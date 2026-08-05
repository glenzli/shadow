use std::{
    net::{TcpListener, TcpStream},
    thread,
    time::Duration,
};

use crate::{
    protocol::{LIBRARY_PROTOCOL_REVISION, Request, RequestEnvelope},
    transport::{read_request, write_request},
};

use super::{AuthorizationToken, constant_time_equal, prepare_connection};

#[test]
fn token_is_redacted_and_requires_real_entropy_budget() {
    assert!(AuthorizationToken::parse("short").is_err());
    let token = AuthorizationToken::parse("01234567890123456789012345678901").expect("valid token");
    assert_eq!(format!("{token:?}"), "AuthorizationToken(\"<redacted>\")");
}

#[test]
fn token_comparison_rejects_different_lengths_and_values() {
    assert!(constant_time_equal(b"same", b"same"));
    assert!(!constant_time_equal(b"same", b"diff"));
    assert!(!constant_time_equal(b"same", b"same-longer"));
}

#[test]
fn accepted_nonblocking_stream_is_normalized_before_framed_read() {
    let listener = TcpListener::bind("127.0.0.1:0").expect("bind listener");
    let mut client = TcpStream::connect(listener.local_addr().expect("listener address"))
        .expect("connect client");
    let (mut server, _) = listener.accept().expect("accept connection");
    server
        .set_nonblocking(true)
        .expect("simulate inherited nonblocking mode");
    prepare_connection(&server, Duration::from_secs(1), Duration::from_secs(1))
        .expect("normalize accepted stream");

    let writer = thread::spawn(move || {
        thread::sleep(Duration::from_millis(25));
        write_request(
            &mut client,
            &RequestEnvelope {
                protocol_revision: LIBRARY_PROTOCOL_REVISION,
                authorization: "01234567890123456789012345678901".to_owned(),
                request: Request::ServerInfo,
            },
        )
        .expect("write delayed request");
    });

    let request = read_request(&mut server).expect("blocking read waits for request frame");
    assert!(matches!(request.request, Request::ServerInfo));
    writer.join().expect("join writer");
}
