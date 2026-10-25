//! Windows ownership for private RAW staging and unpublished model output.
//!
//! Set the directory before admitting a decoder, so newly created payloads inherit
//! only current-user/SYSTEM access. Recheck and protect exact regular file objects
//! before transferring their read-only/writable handles to the official SDK.

use std::{
    ffi::c_void,
    fs::OpenOptions,
    io,
    mem::{MaybeUninit, size_of},
    os::windows::{
        fs::OpenOptionsExt as _,
        io::{AsRawHandle as _, FromRawHandle as _, OwnedHandle},
    },
    path::Path,
    ptr,
};

use windows_sys::Win32::{
    Foundation::LocalFree,
    Security::{
        Authorization::{
            ConvertSidToStringSidW, ConvertStringSecurityDescriptorToSecurityDescriptorW,
            SDDL_REVISION_1, SE_FILE_OBJECT, SetSecurityInfo,
        },
        DACL_SECURITY_INFORMATION, GetSecurityDescriptorDacl, GetTokenInformation,
        OWNER_SECURITY_INFORMATION, PROTECTED_DACL_SECURITY_INFORMATION, TOKEN_QUERY, TOKEN_USER,
        TokenUser,
    },
    Storage::FileSystem::{
        BY_HANDLE_FILE_INFORMATION, FILE_ATTRIBUTE_DIRECTORY, FILE_ATTRIBUTE_REPARSE_POINT,
        FILE_FLAG_BACKUP_SEMANTICS, FILE_FLAG_OPEN_REPARSE_POINT, FILE_READ_ATTRIBUTES,
        GetFileInformationByHandle, READ_CONTROL, WRITE_DAC, WRITE_OWNER,
    },
    System::Threading::{GetCurrentProcess, OpenProcessToken},
};

#[derive(Debug)]
struct LocalAllocation(*mut c_void);

impl Drop for LocalAllocation {
    fn drop(&mut self) {
        // SAFETY: these buffers are returned by the documented LocalAlloc-based
        // SID/SDDL conversion functions and remain owned until this scope ends.
        unsafe {
            let _ = LocalFree(self.0);
        }
    }
}

pub(crate) fn directory(path: &Path) -> io::Result<()> {
    restrict(path, true)
}
pub(crate) fn file(path: &Path) -> io::Result<()> {
    restrict(path, false)
}

fn restrict(path: &Path, directory: bool) -> io::Result<()> {
    let object = OpenOptions::new()
        .read(true)
        .access_mode(READ_CONTROL | WRITE_DAC | WRITE_OWNER | FILE_READ_ATTRIBUTES)
        .custom_flags(FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT)
        .open(path)?;
    let mut information = MaybeUninit::<BY_HANDLE_FILE_INFORMATION>::zeroed();
    // SAFETY: object owns a live file handle and information is writable storage
    // for the API's complete fixed-size result.
    if unsafe { GetFileInformationByHandle(object.as_raw_handle(), information.as_mut_ptr()) } == 0
    {
        return Err(io::Error::last_os_error());
    }
    // SAFETY: the preceding successful call initialized this complete structure.
    let information = unsafe { information.assume_init() };
    if information.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT != 0
        || (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY != 0) != directory
        || (!directory && information.nNumberOfLinks != 1)
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "private staging requires an exact directory or singly linked regular file",
        ));
    }

    let mut token = ptr::null_mut();
    // SAFETY: the current-process pseudo handle is valid; token is writable and
    // ownership of a successful token handle immediately enters OwnedHandle.
    if unsafe { OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &mut token) } == 0 {
        return Err(io::Error::last_os_error());
    }
    let token = unsafe { OwnedHandle::from_raw_handle(token) };
    let mut required = 0;
    // SAFETY: a zero-length probe writes only required. No token data is read yet.
    unsafe {
        GetTokenInformation(
            token.as_raw_handle(),
            TokenUser,
            ptr::null_mut(),
            0,
            &mut required,
        );
    }
    if required < u32::try_from(size_of::<TOKEN_USER>()).unwrap() || required > 65_536 {
        return Err(io::Error::last_os_error());
    }
    // usize storage supplies TOKEN_USER's pointer alignment and retains the SID
    // referenced by its result until SetSecurityInfo has finished copying it.
    let mut user = vec![0usize; (required as usize).div_ceil(size_of::<usize>())];
    if unsafe {
        GetTokenInformation(
            token.as_raw_handle(),
            TokenUser,
            user.as_mut_ptr().cast(),
            required,
            &mut required,
        )
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    let sid = unsafe { (*user.as_ptr().cast::<TOKEN_USER>()).User.Sid };
    let mut sid_text = ptr::null_mut();
    if unsafe { ConvertSidToStringSidW(sid, &mut sid_text) } == 0 {
        return Err(io::Error::last_os_error());
    }
    let sid_allocation = LocalAllocation(sid_text.cast());
    // SID text is a bounded Windows-generated NUL-terminated SDDL identifier.
    let mut sid_length = 0;
    while sid_length < 256 && unsafe { *sid_text.add(sid_length) } != 0 {
        sid_length += 1;
    }
    if sid_length == 256 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Windows SID exceeds its bound",
        ));
    }
    let sid_string =
        String::from_utf16(unsafe { std::slice::from_raw_parts(sid_text, sid_length) })
            .map_err(|_| io::Error::new(io::ErrorKind::InvalidData, "invalid Windows SID text"))?;
    drop(sid_allocation);
    let sddl = format!("D:P(A;OICI;FA;;;{sid_string})(A;OICI;FA;;;SY)")
        .encode_utf16()
        .chain(Some(0))
        .collect::<Vec<_>>();
    let mut descriptor = ptr::null_mut();
    if unsafe {
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.as_ptr(),
            SDDL_REVISION_1,
            &mut descriptor,
            ptr::null_mut(),
        )
    } == 0
    {
        return Err(io::Error::last_os_error());
    }
    let _descriptor = LocalAllocation(descriptor);
    let mut dacl = ptr::null_mut();
    let (mut present, mut defaulted) = (0, 0);
    if unsafe { GetSecurityDescriptorDacl(descriptor, &mut present, &mut dacl, &mut defaulted) }
        == 0
    {
        return Err(io::Error::last_os_error());
    }
    if present == 0 || dacl.is_null() {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "private staging DACL is missing",
        ));
    }
    // SAFETY: the open object was checked without following a reparse point.
    // SID and ACL buffers remain alive; Windows copies their contents.
    let result = unsafe {
        SetSecurityInfo(
            object.as_raw_handle(),
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION
                | DACL_SECURITY_INFORMATION
                | PROTECTED_DACL_SECURITY_INFORMATION,
            sid,
            ptr::null_mut(),
            dacl,
            ptr::null(),
        )
    };
    if result != 0 {
        return Err(io::Error::from_raw_os_error(result as i32));
    }
    Ok(())
}

#[cfg(test)]
mod tests;
