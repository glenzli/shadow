use super::validate;

#[test]
fn rejects_platform_aliases_and_non_ascii_names() {
    for path in [
        "C:/model.onnx",
        "//server/model.onnx",
        r"\\server\model.onnx",
        "weights/model:stream",
        "weights/CON.txt",
        "weights/com1",
        "weights/name.",
        "weights/name ",
        " weights/model.onnx",
        "weights/ model.onnx",
        "weights/model~1.onnx",
        "weights/~model.onnx",
        "weights/mo\u{0301}del.onnx",
    ] {
        assert!(!validate(path), "{path:?} must fail closed");
    }
    assert!(validate("weights/model.onnx"));
}
