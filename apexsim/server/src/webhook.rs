//! A finished race's classification posted to a webhook.
//!
//! `[records] webhook_url` names an `http://` or `https://` endpoint; each
//! race result the server writes (`crate::results`) is also POSTed there,
//! off the game loop. `webhook_format = "json"` posts the result as written
//! (`RaceResult`'s JSON); `"discord"` posts a Discord message
//! (`{"content": ...}`): the track, the distance and one line a car. HTTPS
//! is checked against the Mozilla roots built in (`webpki-roots`); a failed
//! post is logged and dropped, never retried.

use std::sync::Arc;
use std::time::Duration;

use bytes::Bytes;
use http_body_util::{BodyExt, Full};
use hyper::Uri;
use hyper_util::rt::TokioIo;
use serde::{Deserialize, Serialize};
use tokio::net::TcpStream;

use crate::results::RaceResult;

/// What a webhook post carries.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum WebhookFormat {
    /// The result's own JSON, as in `records/results/`.
    #[default]
    Json,
    /// A Discord message: `{"content": "..."}`.
    Discord,
}

/// The longest a post may take, connect to response.
const TIMEOUT: Duration = Duration::from_secs(10);

/// Discord caps a message's content at 2000 characters.
const DISCORD_MAX_CHARS: usize = 2000;

/// The post's body for `format`.
pub fn body(result: &RaceResult, format: WebhookFormat) -> String {
    match format {
        WebhookFormat::Json => serde_json::to_string(result).unwrap_or_default(),
        WebhookFormat::Discord => {
            serde_json::json!({ "content": discord_message(result) }).to_string()
        }
    }
}

/// A race time as `m:ss.mmm` (or `h:mm:ss.mmm`).
fn race_time(ms: u32) -> String {
    let (h, m, s, ms) = (ms / 3_600_000, ms / 60_000 % 60, ms / 1000 % 60, ms % 1000);
    if h > 0 {
        format!("{h}:{m:02}:{s:02}.{ms:03}")
    } else {
        format!("{m}:{s:02}.{ms:03}")
    }
}

/// The classification as a Discord message: a heading, then a line a car,
/// cut to Discord's length with the cars left out counted.
pub fn discord_message(result: &RaceResult) -> String {
    let distance = match (result.lap_limit, result.race_seconds) {
        (_, Some(s)) => format!("{} min", s / 60),
        (Some(laps), None) => format!("{laps} laps"),
        _ => String::new(),
    };
    let mut out = format!("**Race result: {}**", result.track_name);
    if !distance.is_empty() {
        out.push_str(&format!(" ({distance})"));
    }
    out.push('\n');
    for (i, car) in result.cars.iter().enumerate() {
        let gap = match (car.status, car.race_time_ms, car.gap_ms) {
            ("finished", Some(t), None) if car.position == 1 => race_time(t),
            ("finished", _, Some(gap)) => format!("+{}.{:03}", gap / 1000, gap % 1000),
            ("finished", _, None) if car.laps_down > 0 => {
                format!(
                    "+{} lap{}",
                    car.laps_down,
                    if car.laps_down == 1 { "" } else { "s" }
                )
            }
            ("finished", Some(t), None) => race_time(t),
            (status, ..) => status.to_uppercase(),
        };
        let mut line = format!("{}. {} {}", car.position, car.name, gap);
        if let Some(best) = car.best_lap_ms {
            line.push_str(&format!(" (best {})", race_time(best)));
        }
        if car.penalty_s > 0 {
            line.push_str(&format!(" +{} s pen", car.penalty_s));
        }
        line.push('\n');
        let more = format!("... and {} more\n", result.cars.len() - i);
        if out.chars().count() + line.chars().count() + more.chars().count() > DISCORD_MAX_CHARS {
            out.push_str(&more);
            break;
        }
        out.push_str(&line);
    }
    out
}

/// The TLS client config: the built-in Mozilla roots.
fn tls_config() -> Arc<rustls::ClientConfig> {
    let roots = rustls::RootCertStore {
        roots: webpki_roots::TLS_SERVER_ROOTS.to_vec(),
    };
    Arc::new(
        rustls::ClientConfig::builder_with_provider(Arc::new(
            rustls::crypto::ring::default_provider(),
        ))
        .with_safe_default_protocol_versions()
        .expect("ring supports the default protocol versions")
        .with_root_certificates(roots)
        .with_no_client_auth(),
    )
}

/// POST `body` as JSON to `url`; the response's status code.
pub async fn post(url: &str, body: String) -> Result<u16, String> {
    tokio::time::timeout(TIMEOUT, post_inner(url, body))
        .await
        .map_err(|_| format!("no response within {} s", TIMEOUT.as_secs()))?
}

async fn post_inner(url: &str, body: String) -> Result<u16, String> {
    let uri: Uri = url.parse().map_err(|e| format!("bad url: {e}"))?;
    let https = match uri.scheme_str() {
        Some("https") => true,
        Some("http") => false,
        _ => return Err("the url must be http:// or https://".into()),
    };
    let host = uri.host().ok_or("the url has no host")?.to_string();
    let port = uri.port_u16().unwrap_or(if https { 443 } else { 80 });
    let path = uri
        .path_and_query()
        .map_or("/".to_string(), |p| p.as_str().to_string());
    let request = hyper::Request::post(path)
        .header(hyper::header::HOST, &host)
        .header(hyper::header::CONTENT_TYPE, "application/json")
        .header(hyper::header::USER_AGENT, "apexsim-server")
        .body(Full::new(Bytes::from(body)))
        .map_err(|e| e.to_string())?;

    let tcp = TcpStream::connect((host.as_str(), port))
        .await
        .map_err(|e| format!("connect: {e}"))?;
    if https {
        let name = rustls::pki_types::ServerName::try_from(host.clone())
            .map_err(|e| format!("bad host name: {e}"))?;
        let tls = tokio_rustls::TlsConnector::from(tls_config())
            .connect(name, tcp)
            .await
            .map_err(|e| format!("tls: {e}"))?;
        send(TokioIo::new(tls), request).await
    } else {
        send(TokioIo::new(tcp), request).await
    }
}

async fn send<T>(io: T, request: hyper::Request<Full<Bytes>>) -> Result<u16, String>
where
    T: hyper::rt::Read + hyper::rt::Write + Unpin + Send + 'static,
{
    let (mut sender, conn) = hyper::client::conn::http1::handshake(io)
        .await
        .map_err(|e| format!("http: {e}"))?;
    tokio::spawn(conn);
    let response = sender
        .send_request(request)
        .await
        .map_err(|e| format!("http: {e}"))?;
    let status = response.status().as_u16();
    // Drain the body so the connection closes cleanly.
    let _ = response.into_body().collect().await;
    Ok(status)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::results::ClassifiedCar;

    fn car(position: u8, name: &str, status: &'static str) -> ClassifiedCar {
        ClassifiedCar {
            position,
            player_id: uuid::Uuid::from_u128(position as u128),
            name: name.to_string(),
            is_ai: true,
            car_config_id: uuid::Uuid::nil(),
            status,
            laps: 5,
            best_lap_ms: Some(81_234),
            race_time_ms: (status == "finished").then_some(412_345),
            gap_ms: (position == 2).then_some(1_234),
            laps_down: 0,
            missed_stops: false,
            penalty_s: 0,
        }
    }

    fn result(cars: Vec<ClassifiedCar>) -> RaceResult {
        RaceResult {
            session_id: "abc".into(),
            track_name: "Monzarella".into(),
            track_stem: Some("Monza".into()),
            finished_at: 1_792_200_000,
            lap_limit: Some(5),
            race_seconds: None,
            cars,
        }
    }

    #[test]
    fn a_discord_message_lists_the_classification() {
        let r = result(vec![
            car(1, "Rex Thunder", "finished"),
            car(2, "Luna Swift", "finished"),
            car(3, "Max Voltage", "retired"),
        ]);
        let msg = discord_message(&r);
        let lines: Vec<&str> = msg.lines().collect();
        assert_eq!(lines[0], "**Race result: Monzarella** (5 laps)");
        assert_eq!(lines[1], "1. Rex Thunder 6:52.345 (best 1:21.234)");
        assert_eq!(lines[2], "2. Luna Swift +1.234 (best 1:21.234)");
        assert_eq!(lines[3], "3. Max Voltage RETIRED (best 1:21.234)");
        let body: serde_json::Value =
            serde_json::from_str(&body(&r, WebhookFormat::Discord)).unwrap();
        assert_eq!(body["content"], msg);
    }

    #[test]
    fn a_long_field_is_cut_to_discords_length() {
        let cars = (1..=200)
            .map(|p| car(p as u8, &"N".repeat(30), "finished"))
            .collect();
        let msg = discord_message(&result(cars));
        assert!(msg.chars().count() <= DISCORD_MAX_CHARS);
        assert!(msg.contains("more\n"));
    }

    #[tokio::test]
    async fn a_result_is_posted_to_a_test_endpoint() {
        use tokio::io::{AsyncReadExt, AsyncWriteExt};
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let port = listener.local_addr().unwrap().port();
        let server = tokio::spawn(async move {
            let (mut sock, _) = listener.accept().await.unwrap();
            let mut got = Vec::new();
            let mut buf = [0u8; 4096];
            // Read until the whole body (Content-Length) is in.
            loop {
                let n = sock.read(&mut buf).await.unwrap();
                got.extend_from_slice(&buf[..n]);
                let text = String::from_utf8_lossy(&got).to_string();
                if let Some(split) = text.find("\r\n\r\n") {
                    let len: usize = text
                        .lines()
                        .find_map(|l| {
                            l.to_ascii_lowercase()
                                .strip_prefix("content-length: ")
                                .map(|v| v.trim().parse().unwrap())
                        })
                        .unwrap();
                    if got.len() >= split + 4 + len {
                        break;
                    }
                }
                if n == 0 {
                    break;
                }
            }
            sock.write_all(b"HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n")
                .await
                .unwrap();
            String::from_utf8(got).unwrap()
        });
        let r = result(vec![car(1, "Rex Thunder", "finished")]);
        let status = post(
            &format!("http://127.0.0.1:{port}/hook?x=1"),
            body(&r, WebhookFormat::Json),
        )
        .await
        .unwrap();
        assert_eq!(status, 204);
        let request = server.await.unwrap();
        assert!(
            request.starts_with("POST /hook?x=1 HTTP/1.1\r\n"),
            "{request}"
        );
        let json = &request[request.find("\r\n\r\n").unwrap() + 4..];
        let back: serde_json::Value = serde_json::from_str(json).unwrap();
        assert_eq!(back["track_name"], "Monzarella");
        assert_eq!(back["cars"][0]["name"], "Rex Thunder");
    }

    #[tokio::test]
    async fn a_bad_url_is_refused() {
        assert!(post("ftp://example.com/x", String::new()).await.is_err());
        assert!(post("not a url", String::new()).await.is_err());
    }

    #[test]
    fn the_tls_config_has_roots() {
        let _ = tls_config();
    }
}
