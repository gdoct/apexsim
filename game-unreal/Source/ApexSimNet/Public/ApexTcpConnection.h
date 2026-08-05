#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"

class FRunnableThread;
class FSocket;
class ISocketSubsystem;

/** Why the socket thread stopped. Surfaced to the UI verbatim. */
struct FApexDisconnectReason
{
	FString Text;
	/** True when the failure happened before/during connect rather than mid-session. */
	bool bDuringConnect = false;
};

/**
 * Owns the TCP socket to the ApexSim server and runs it on a dedicated thread.
 *
 * One thread services both directions: connect, send queued frames, poll for
 * readable data, extract frames, decode. Decoding happens here rather than on
 * the game thread precisely because LobbyState is a ~250 KB message arriving
 * every 2 seconds.
 *
 * Ownership: UApexNetSubsystem creates one of these per connection attempt and
 * destroys it via Shutdown(). Nothing else may touch the socket.
 */
class APEXSIMNET_API FApexTcpConnection : public FRunnable
{
public:
	FApexTcpConnection(const FString& InHost, int32 InPort, const FString& InToken, const FString& InPlayerName);
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

	bool ConnectSocket(FString& OutError);
	/** Writes everything currently queued. Returns false on a socket error. */
	bool FlushOutbound();
	/** Sends a whole buffer, looping over partial sends. */
	bool SendAll(const TArray<uint8>& Bytes);
	/** Appends readable bytes to ReceiveBuffer. Returns false on error or clean close. */
	bool ReceiveAvailable(FString& OutError);
	/** Extracts and decodes every complete frame in ReceiveBuffer. */
	bool ExtractFrames(FString& OutError);
	void SetDisconnectReason(const FString& Text, bool bDuringConnect);
	void DestroySocket();

	const FString Host;
	const int32 Port;
	const FString Token;
	const FString PlayerName;

	FSocket* Socket = nullptr;
	ISocketSubsystem* SocketSubsystem = nullptr;
	FRunnableThread* Thread = nullptr;

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
