use crate::protocol::{
    CapabilityAvailability, LIBRARY_PROTOCOL_VERSION, Request, RequestEnvelope, ResponseHeader,
    ResponseValue, ServerCapabilities, ServerId, ServerInfo,
};

use super::{read_request, read_response, write_request, write_response};

#[test]
fn framed_request_and_binary_response_round_trip() {
    let request = RequestEnvelope {
        protocol_version: LIBRARY_PROTOCOL_VERSION,
        authorization: "01234567890123456789012345678901".to_owned(),
        request: Request::ServerInfo,
    };
    let mut request_wire = Vec::new();
    write_request(&mut request_wire, &request).expect("write request");
    let decoded = read_request(&mut request_wire.as_slice()).expect("read request");
    assert!(matches!(decoded.request, Request::ServerInfo));

    let header = ResponseHeader {
        protocol_version: LIBRARY_PROTOCOL_VERSION,
        value: Ok(ResponseValue::ServerInfo(ServerInfo {
            protocol_version: LIBRARY_PROTOCOL_VERSION,
            server_id: ServerId(uuid::Uuid::now_v7()),
            display_name: "Studio Mac".to_owned(),
            capabilities: ServerCapabilities {
                serves_embedded_previews: CapabilityAvailability::Available,
                serves_generated_proxies: CapabilityAvailability::Available,
                serves_originals: CapabilityAvailability::Available,
                private_preview_provider: CapabilityAvailability::Unavailable,
                maximum_page_size: 256,
                maximum_original_chunk_bytes: 4 * 1_024 * 1_024,
            },
        })),
        body_byte_len: 3,
    };
    let mut response_wire = Vec::new();
    write_response(&mut response_wire, &header, b"raw").expect("write response");
    let (decoded, body) = read_response(&mut response_wire.as_slice()).expect("read response");
    assert!(matches!(decoded.value, Ok(ResponseValue::ServerInfo(_))));
    assert_eq!(body, b"raw");
}
