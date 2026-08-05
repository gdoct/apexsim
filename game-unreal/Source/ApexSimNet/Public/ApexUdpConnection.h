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
	static constexpr int32 MaxDatagramBytes = 2048;

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
	/** Latest server tick seen, echoed back so the server can measure latency. */
	FThreadSafeCounter LastServerTick;

	/** Worker produces, game thread consumes. */
	TQueue<FApexTelemetryFrame, EQueueMode::Spsc> TelemetryQueue;

	/** Written by the game thread, read by the worker. */
	mutable FCriticalSection InputLock;
	FApexPlayerInput PendingInput;

	TArray<uint8> ReceiveBuffer;
};
