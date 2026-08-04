use std::{net::SocketAddr, time::Duration};

use crate::AuthorizationToken;

use super::{LibraryClient, LibraryClientConfig};

#[test]
fn client_configuration_keeps_connection_and_operation_timeouts_separate() {
    let address: SocketAddr = "127.0.0.1:31415".parse().expect("socket address");
    let token = AuthorizationToken::parse("01234567890123456789012345678901").expect("token");
    let mut config = LibraryClientConfig::new(address, token);
    config.connect_timeout = Duration::from_secs(2);
    config.read_timeout = Duration::from_secs(9);
    let client = LibraryClient::new(config.clone());
    assert_eq!(client.config.connect_timeout, Duration::from_secs(2));
    assert_eq!(client.config.read_timeout, Duration::from_secs(9));
}
