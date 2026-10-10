#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "ApexTls.h"
#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"

class FApexTlsSession;
class FRunnableThread;
class FSocket;
class ISocketSubsystem;

/** Why the socket thread stopped. Surfaced to the UI verbatim. */
struct FApexDisconnectReason
{
	FString Text;
	/** True when the failure happened before/during connect rather than mid-session. */
	bool bDuringConnect = false;
	/**
	 * Retrying the same server will not help: its TLS certificate failed the
	 * check settings.yml asks for. The subsystem stops reconnecting.
	 */
	bool bPermanent = false;
};

/**
 * Owns the TCP socket to the ApexSim server and runs it on a dedicated thread.
 *
 * One thread services both directions: connect, send queued frames, poll for
 * readable data, extract frames, decode. TLS (FApexTlsOptions, settings.yml
 * `server.tls`) sits between the socket and the framing on the same thread:
 * an OpenSSL session on memory BIOs (FApexTlsSession) that the socket feeds
 * and drains, so the `[4-byte BE length][msgpack]` frames are the same either
 * way. In `auto` a server that drops the TLS hello unanswered (a plaintext
 * ApexSim server does) is connected to again in plaintext. Decoding happens here rather than on
 * the game thread precisely because LobbyState is a ~250 KB message arriving
 * every 2 seconds.
 *
 * Ownership: UApexNetSubsystem creates one of these per connection attempt and
 * destroys it via Shutdown(). Nothing else may touch the socket.
 */
class APEXSIMNET_API FApexTcpConnection : public FRunnable
{
public:
	FApexTcpConnection(const FString& InHost, int32 InPort, const FString& InToken, const FString& InPlayerName,
		const FApexTlsOptions& InTls = FApexTlsOptions(), const FString& InResumeToken = FString());
	virtual ~FApexTcpConnection() override;

	/** Spawns the worker thread. Returns false only if thread creation itself failed. */
	bool Start();

	/**
	 * Stops the worker and joins it. Safe to call more than once, and safe to
	 * call from the game thread — the socket is only ever destroyed on the
	 * worker thread, in Exit().
	 */
	void Shutdown();

	/** Queues a payload to send. The 4-byte length prefix is added here. */
	void Send(TArray<uint8>&& Payload);

	/** Pops one decoded message. Returns false when the queue is empty. */
	bool PopMessage(FApexServerMessage& OutMessage);

	/** Pops the disconnect reason, if the worker has finished. */
	bool PopDisconnectReason(FApexDisconnectReason& OutReason);

	bool IsConnected() const { return bConnected; }

	/** True once connected over TLS (false in plaintext, and before). */
	bool IsEncrypted() const { return bEncrypted; }

	// FRunnable
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

private:
	/** Server caps a frame at 1 MB; this is defensive headroom over the ~250 KB LobbyState. */
	static constexpr uint32 MaxFrameBytes = 4u * 1024u * 1024u;
	static constexpr int32 ReceiveBufferBytes = 1 << 20;
	static constexpr int32 SendBufferBytes = 1 << 16;
	static constexpr int32 PollIntervalMs = 50;

	/** No reply to a ClientHello in this long is a failed handshake. */
	static constexpr double TlsHandshakeTimeoutSeconds = 10.0;

	bool ConnectSocket(FString& OutError);
	/**
	 * Runs the TLS handshake on the connected socket and checks the server's
	 * certificate. On failure says how it failed, which decides the fallback.
	 */
	bool HandshakeTls(FString& OutError, EApexTlsFailure& OutFailure);
	/** Writes everything currently queued. Returns false on a socket error. */
	bool FlushOutbound();
	/** Sends a frame: through TLS when the connection has it. */
	bool SendAll(const TArray<uint8>& Bytes);
	/** Sends bytes as they are on the socket, looping over partial sends. */
	bool SendRaw(const uint8* Bytes, int32 Num);
	/** Sends whatever ciphertext the TLS session has waiting. */
	bool FlushCiphertext();
	/**
	 * Reads what the socket has (call once Wait said it is readable): one Recv,
	 * which is what notices a closed connection, then whatever else is pending.
	 * False on an error or a clean close.
	 */
	bool RecvRaw(TArray<uint8>& Out, FString& OutError);
	/** Appends readable (decrypted) bytes to ReceiveBuffer. Returns false on error or clean close. */
	bool ReceiveAvailable(FString& OutError);
	/** Extracts and decodes every complete frame in ReceiveBuffer. */
	bool ExtractFrames(FString& OutError);
	void SetDisconnectReason(const FString& Text, bool bDuringConnect, bool bPermanent = false);
	void DestroySocket();

	const FString Host;
	const int32 Port;
	const FString Token;
	const FString PlayerName;
	const FApexTlsOptions TlsOptions;
	/** Sent in Authenticate to be the same player again; empty for a new one. */
	const FString ResumeToken;

	/** Worker-thread only. Set while the connection runs over TLS. */
	TUniquePtr<FApexTlsSession> Tls;
	FThreadSafeBool bEncrypted{false};

	FSocket* Socket = nullptr;
	ISocketSubsystem* SocketSubsystem = nullptr;
	FRunnableThread* Thread = nullptr;
	/**
	 * Guards `Socket` between the game thread's Stop() and the worker, which
	 * creates it in ConnectSocket() and destroys it on a failed connect or in
	 * Exit(). Stop() reading a pointer the worker is freeing crashed on exit.
	 */
	FCriticalSection SocketLock;

	FThreadSafeBool bStopRequested{false};
	FThreadSafeBool bConnected{false};

	/** Game thread produces, worker consumes. */
	TQueue<TArray<uint8>, EQueueMode::Spsc> OutboundQueue;
	/** Worker produces, game thread consumes. */
	TQueue<FApexServerMessage, EQueueMode::Spsc> InboundQueue;
	TQueue<FApexDisconnectReason, EQueueMode::Spsc> DisconnectQueue;

	/** Worker-thread only. Rolling buffer of bytes not yet forming a whole frame. */
	TArray<uint8> ReceiveBuffer;
	bool bDisconnectReported = false;
};
