use std::ffi::OsString;

use super::debug_arguments;

#[test]
fn debug_launch_preserves_selection_and_uses_independent_launch_history() {
    let default = debug_arguments(vec![]);
    assert_eq!(
        default,
        vec![
            OsString::from("--profile-launch-context"),
            OsString::from("debug")
        ]
    );
    for args in [
        vec![OsString::from("--profile"), OsString::from("daily")],
        vec![OsString::from("--profile=debug")],
        vec![
            OsString::from("--profile-root"),
            OsString::from("/tmp/separate"),
        ],
        vec![OsString::from("--profile-root=/tmp/separate")],
    ] {
        let result = debug_arguments(args.clone());
        assert_eq!(&result[..args.len()], args.as_slice());
        assert_eq!(&result[args.len()..], default.as_slice());
    }
    let explicit_context = vec![OsString::from("--profile-launch-context=normal")];
    assert_eq!(debug_arguments(explicit_context.clone()), explicit_context);
}
