#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/ThreadSafeCounter.h"

class FInternetAddr;
class FRunnableThread;
class FSocket;
class ISocketSubsystem;

/**
 * The UDP half of protocol v2: telemetry in, player input out.
 *
 * Datagrams are bare MessagePack payloads with no length prefix — unlike the
 * TCP stream, where every frame is preceded by a 4-byte length.
 *
 * The connection is not usable until the server has bound this socket's source
 * address to the TCP session. That happens by sending `UdpHandshake` carrying
 * the token from `AuthSuccess` and waiting for `UdpHandshakeAck`. Datagrams get
 * lost, so the handshake is re-sent on an interval until it is acknowledged.
 *
 * Everything runs on one worker thread; the game thread only touches the
 * lock-free queues and the atomics.
 */
class APEXSIMNET_API FApexUdpConnection : public FRunnable
{
public:
	FApexUdpConnection(const FString& InHost, int32 InUdpPort, const FString& InUdpToken);
	virtual ~FApexUdpConnection() override;

	bool Start();

	/** Stops the worker and joins it. Safe to call more than once. */
	void Shutdown();

	/** True once `UdpHandshakeAck` has come back. */
	bool IsHandshakeComplete() const { return bHandshakeComplete; }

	/** Replaces the input the worker sends each interval. Cheap; call per frame. */
	void SetPlayerInput(const FApexPlayerInput& Input);

	/** Pops one decoded telemetry frame. Returns false when the queue is empty. */
	bool PopTelemetry(FApexTelemetryFrame& OutFrame);

	/**
	 * Throws away every telemetry frame queued so far and returns how many.
	 * Called when a session is joined: a frame that arrived before the join
	 * belongs to the session just left.
	 */
	int32 DiscardQueuedTelemetry();

	/** Pops one decoded force-feedback message. Returns false when the queue is empty. */
	bool PopDriverFeedback(FApexDriverFeedback& OutFeedback);

	/** Total datagrams received, for diagnostics. */
	int32 GetReceivedDatagramCount() const { return ReceivedDatagrams.GetValue(); }

	// FRunnable
	virtual bool Init() override;
	virtual uint32 Run() override;
	virtual void Stop() override;
	virtual void Exit() override;

private:
	/** Server caps UDP input at 300/s; 60 is plenty and stays well clear. */
	static constexpr float InputSendHz = 60.0f;
	static constexpr int32 HandshakeRetryMs = 250;
	static constexpr int32 PollIntervalMs = 4;
	/**
	 * The largest UDP payload there is. Telemetry grows with the field (about
	 * 160 bytes a car) and with its values, since MessagePack widens an int
	 * as it grows: a 13-car frame crossed an old 2048-byte buffer two seconds
	 * after the green light, and every frame after it was dropped.
	 */
	static constexpr int32 MaxDatagramBytes = 65536;

	bool CreateSocket(FString& OutError);
	void SendHandshake();
	void SendPlayerInput();
	/** Drains everything readable, decoding as it goes. */
	void ReceiveAvailable();
	void DestroySocket();

	const FString Host;
	const int32 UdpPort;
	const FString UdpToken;

	FSocket* Socket = nullptr;
	ISocketSubsystem* SocketSubsystem = nullptr;
	FRunnableThread* Thread = nullptr;
	TSharedPtr<FInternetAddr> ServerAddr;

	FThreadSafeBool bStopRequested{false};
	FThreadSafeBool bHandshakeComplete{false};
	FThreadSafeCounter ReceivedDatagrams;
	/** Worker thread only: the first failed receive or undecodable datagram is a warning, the rest are verbose. */
	bool bLoggedReceiveFailure = false;
	bool bLoggedDecodeFailure = false;
	/** Latest server tick seen, echoed back so the server can measure latency. */
	FThreadSafeCounter LastServerTick;

	/** Worker produces, game thread consumes. */
	TQueue<FApexTelemetryFrame, EQueueMode::Spsc> TelemetryQueue;
	TQueue<FApexDriverFeedback, EQueueMode::Spsc> DriverFeedbackQueue;

	/** Written by the game thread, read by the worker. */
	mutable FCriticalSection InputLock;
	FApexPlayerInput PendingInput;

	TArray<uint8> ReceiveBuffer;
};
