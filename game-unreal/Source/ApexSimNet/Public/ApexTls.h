#pragma once

#include "CoreMinimal.h"

/**
 * TLS on the TCP connection to the server (settings.yml `server.tls`).
 *
 * The server wraps its TCP port in TLS when it has a certificate
 * (`[network] tls_cert_path` / `tls_key_path`; `require_tls`, true by default,
 * refuses to start without one). The game's FApexTcpConnection speaks TLS 1.2+
 * through the OpenSSL the engine ships, on memory BIOs over the same socket and
 * thread, so the `[4-byte BE length][msgpack]` framing above it is unchanged.
 * UDP (telemetry and input) is not encrypted; the token that binds it arrives
 * in AuthSuccess over the TLS connection.
 */
enum class EApexTlsMode : uint8
{
	/** Try TLS; reconnect in plaintext only when the server plainly does not speak it. */
	Auto,
	/** TLS or nothing. */
	On,
	/** Plaintext, as before. */
	Off,
};

struct FApexTlsOptions
{
	EApexTlsMode Mode = EApexTlsMode::Auto;

	/**
	 * Check the certificate against the system's CA roots and the host name.
	 * Ignored when a fingerprint is pinned (the pin is the check). False is for
	 * development only: the connection is encrypted but anyone in the middle
	 * can read it.
	 */
	bool bVerify = true;

	/**
	 * SHA-256 of the server's certificate (DER), 64 hex digits, colons
	 * optional; what `openssl x509 -noout -fingerprint -sha256` prints. Set,
	 * it replaces the CA check: the server must present exactly that
	 * certificate, which is how a self-signed one is trusted. A pin also makes
	 * `auto` behave as `on` (a pinned server is expected to speak TLS).
	 */
	FString Fingerprint;

	bool IsPinned() const { return !Fingerprint.IsEmpty(); }
};

/** How a TLS attempt failed, as far as the plaintext fallback cares. */
enum class EApexTlsFailure : uint8
{
	None,
	/**
	 * The server closed or reset the connection before sending a byte. A
	 * plaintext ApexSim server reads a ClientHello's first four bytes as a frame
	 * length of ~370 MB, over its 1 MB cap, and drops the connection: this.
	 */
	ClosedBeforeReply,
	/** The first bytes back are not a TLS record: something plaintext answered. */
	NotTls,
	/** A TLS peer, but the handshake failed (an alert, no common protocol). */
	Handshake,
	/** The handshake completed but the certificate is not trusted. */
	Untrusted,
	/** No answer within the handshake timeout. */
	Timeout,
	/** OpenSSL could not be set up on this machine. */
	Setup,
};

namespace ApexTls
{
	/** `auto`, `on`, `off` (also true/yes and false/no for on/off), any case. */
	APEXSIMNET_API bool ParseMode(const FString& Text, EApexTlsMode& Out);

	APEXSIMNET_API const TCHAR* ModeName(EApexTlsMode Mode);

	/**
	 * A SHA-256 fingerprint in any of the usual spellings (colons, spaces or
	 * dashes between bytes, either case, an optional `sha256:` prefix) as 64
	 * upper-case hex digits. False when it is not 32 bytes of hex.
	 */
	APEXSIMNET_API bool NormaliseFingerprint(const FString& Text, FString& Out);

	/** NormaliseFingerprint, then spelt `AB:CD:...`. False (Out untouched) when invalid. */
	APEXSIMNET_API bool CanonicalFingerprint(const FString& Text, FString& Out);

	/** A digest as `AB:CD:...`, the spelling settings.yml and the logs use. */
	APEXSIMNET_API FString FormatFingerprint(TArrayView<const uint8> Digest);

	/** True when `Pinned` (any spelling NormaliseFingerprint takes) is `Digest`. */
	APEXSIMNET_API bool FingerprintMatches(const FString& Pinned, TArrayView<const uint8> Digest);

	/** A TLS record starts with a content type of 20..24 (0x14..0x18). */
	APEXSIMNET_API bool LooksLikeTlsRecord(uint8 FirstByte);

	/**
	 * The first-byte and byte-count reading of a failed handshake: nothing
	 * received is ClosedBeforeReply, a non-TLS first byte is NotTls, anything
	 * else `Otherwise`.
	 */
	APEXSIMNET_API EApexTlsFailure ClassifyFailure(int64 BytesReceived, uint8 FirstByte, EApexTlsFailure Otherwise);

	/**
	 * Whether a failed TLS attempt should be retried in plaintext: only in
	 * `auto`, only without a pin, and only when the server showed it does not
	 * speak TLS (ClosedBeforeReply, NotTls). A certificate that fails its check
	 * never falls back.
	 */
	APEXSIMNET_API bool ShouldFallBackToPlaintext(const FApexTlsOptions& Options, EApexTlsFailure Failure);

	/** Failures that retrying the same server will not cure (the certificate). */
	APEXSIMNET_API bool IsPermanent(EApexTlsFailure Failure);
}
