#pragma once

#include "CoreMinimal.h"
#include "ApexTls.h"

struct ssl_st;
struct ssl_ctx_st;
struct bio_st;

/**
 * One TLS client session over memory BIOs: it never touches a socket. The
 * owner (FApexTcpConnection, on its worker thread) moves bytes: whatever the
 * peer sent goes in with FeedCiphertext, and whatever TakeCiphertext hands back
 * goes out on the wire after every step. Not thread-safe; one thread drives it.
 */
class FApexTlsSession
{
public:
	enum class EStep : uint8
	{
		Done,
		/** Needs more bytes from the peer (send TakeCiphertext's first). */
		WantRead,
		Failed,
	};

	enum class ERead : uint8
	{
		Ok,
		/** The peer sent close_notify. */
		Closed,
		Failed,
	};

	FApexTlsSession() = default;
	~FApexTlsSession();

	FApexTlsSession(const FApexTlsSession&) = delete;
	FApexTlsSession& operator=(const FApexTlsSession&) = delete;

	/**
	 * Makes the context (TLS 1.2 or newer; the system's CA roots when the
	 * options verify without a pin) and the session for `Host` (SNI for a
	 * name, and the name or address the certificate is checked against).
	 */
	bool Init(const FString& Host, const FApexTlsOptions& Options, FString& OutError);

	/** Advances the handshake as far as the bytes fed so far allow. */
	EStep StepHandshake(FString& OutError);

	/**
	 * After the handshake: checks the certificate as the options ask (the pin,
	 * else the CA chain and host name, else nothing). False with the reason,
	 * which names the certificate's fingerprint so it can be pinned.
	 */
	bool CheckPeer(FString& OutError);

	/** Encrypts `Bytes` (ciphertext then waits in TakeCiphertext). */
	bool Write(const uint8* Bytes, int32 Num, FString& OutError);

	/** Decrypts everything the fed bytes hold onto the end of `Out`. */
	ERead ReadAvailable(TArray<uint8>& Out, FString& OutError);

	void FeedCiphertext(const uint8* Bytes, int32 Num);
	/** Moves pending ciphertext onto the end of `Out`; true when there was any. */
	bool TakeCiphertext(TArray<uint8>& Out);

	/** Queues a close_notify (best effort, before the socket closes). */
	void Close();

	/** The server certificate's SHA-256, `AB:CD:...`; empty before the handshake. */
	const FString& PeerFingerprint() const { return Fingerprint; }

	/** "TLSv1.3, TLS_AES_256_GCM_SHA384". */
	FString DescribeCipher() const;

	/** How the certificate was trusted, for the log: "pinned", "CA-verified" or "NOT verified". */
	const TCHAR* TrustDescription() const;

private:
	FString ErrorString(int32 SslResult) const;

	ssl_ctx_st* Context = nullptr;
	ssl_st* Ssl = nullptr;
	bio_st* ReadBio = nullptr;   // owned by Ssl
	bio_st* WriteBio = nullptr;  // owned by Ssl
	FApexTlsOptions Options;
	FString Host;
	FString Fingerprint;
};
