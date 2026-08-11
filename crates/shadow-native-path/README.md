# Shadow native paths

`shadow-native-path` owns the lossless boundary between persisted
`AssetLocation` values and platform-native filesystem paths.

- Unix and macOS persist the original `OsStr` bytes.
- Windows persists the original little-endian UTF-16 code units, including
  unpaired surrogate code units accepted by Windows paths.
- `display_path` is presentation only and is never used to reopen a file.
- A location can be reopened only on the exact platform that created it.

Use the platform-independent Windows-unit codec to validate persisted fixtures
on any host. Use `native_location` and `native_path_from_location` only when
crossing the current host filesystem boundary. Qt or Win32 callers that already
own UTF-16 units can use `windows_location_from_units` without converting them
through UTF-8 or a display string.
