#pragma once

#include "CoreMinimal.h"
#include "ApexProtocolTypes.h"
#include "Containers/Queue.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/ThreadSafeCounter.h"

class FInternetAddr;

/** One input as the UDP worker sent it: its number (`PlayerInput.seq`), the controls, and when. */
struct FApexSentInput
{
	uint32 Seq = 0;
	FApexPlayerInput Input;
	/** FPlatformTime::Seconds when it was handed to the socket (or to the fake-latency queue). */
	double SentSeconds = 0.0;
};
class FRunnableThread;
class FSocket;
class ISocketSubsystem;

/**
 * The UDP half of protocol v2: telemetry in, player input out.
 *
 * Datagrams carry MessagePack payloads with no length prefix — unlike the
 * TCP stream, where every frame is preceded by a 4-byte length. Everything
 * we send is sealed (ApexProtocol::SealUdpDatagram) under the key from
 * `AuthSuccess` with a sequence number that rises with every send; what the
 * server sends arrives plain.
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
	FApexUdpConnection(const FString& InHost, int32 InUdpPort, const FString& InUdpToken, const FString& InUdpKey);
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

	/**
	 * Pops one spectator stream frame as it arrived, framed
	 * (`[u32 length][body]`, ApexSpectatorStream.h). Returns false when the
	 * queue is empty.
	 */
	bool PopSpectatorRecord(TArray<uint8>& OutRecords);
	/** Throws away every queued spectator frame and returns how many. */
	int32 DiscardQueuedSpectatorRecords();

	/** Total datagrams received, for diagnostics. */
	int32 GetReceivedDatagramCount() const { return ReceivedDatagrams.GetValue(); }

	/**
	 * The number (`PlayerInput.seq`) of the input sent last, 0 before the
	 * first; it rises by one with each input sent. Any thread.
	 */
	uint32 GetLastInputSeq() const { return static_cast<uint32>(LastInputSeq.GetValue()); }

	/**
	 * The inputs sent with a number above `AfterSeq`, oldest first, from a
	 * ring of the last SentInputRing sent (about eight seconds). Any thread.
	 */
	void CopySentInputs(uint32 AfterSeq, TArray<FApexSentInput>& Out) const;

	/**
	 * The artificial latency (`apexsim.net.FakeLatencyMs`, a round trip):
	 * each input waits half of it before it goes out and each datagram that
	 * comes in waits the other half before it is decoded, so a ping can be
	 * felt on localhost. 0 is off. The TCP side is not delayed.
	 */
	static int32 GetFakeLatencyMs();

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
	/** Seals and sends one payload; the sequence number rises with each call. */
	void SendSealed(TArrayView<const uint8> Payload);
	/** Drains everything readable, decoding as it goes (or queueing it under a fake latency). */
	void ReceiveAvailable();
	/** Decodes one datagram and queues what it carries. */
	void HandleDatagram(TArrayView<const uint8> Bytes);
	/** Sends the delayed inputs and decodes the delayed datagrams whose time has come. */
	void FlushDelayed(double Now);
	/** Seconds until the next delayed datagram is due; a large number when none is waiting. */
	double SecondsToNextDelayed(double Now) const;
	void DestroySocket();

	const FString Host;
	const int32 UdpPort;
	const FString UdpToken;
	const FString UdpKey;
	/** Worker thread only: the sequence number of the last datagram sent. */
	uint64 SendSeq = 0;
	/** Worker thread only: the number of the last input sent (`PlayerInput.seq`). */
	uint32 InputSeq = 0;
	/** InputSeq, for other threads. */
	FThreadSafeCounter LastInputSeq;

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
	TQueue<TArray<uint8>, EQueueMode::Spsc> SpectatorQueue;

	/** Written by the game thread, read by the worker. */
	mutable FCriticalSection InputLock;
	FApexPlayerInput PendingInput;

	TArray<uint8> ReceiveBuffer;

	/** How many sent inputs CopySentInputs can see: 60 a second. */
	static constexpr int32 SentInputRing = 512;
	mutable FCriticalSection SentInputLock;
	TArray<FApexSentInput> SentInputs;
	int32 SentInputHead = 0;

	/** Worker thread only: datagrams held by the fake latency, oldest first, with when each is due. */
	struct FDelayed
	{
		double DueSeconds = 0.0;
		TArray<uint8> Bytes;
	};
	TArray<FDelayed> DelayedOut;
	TArray<FDelayed> DelayedIn;
};
