#include "ApexUdpConnection.h"

#include "ApexProtocolCodec.h"
#include "ApexSimNetModule.h"
#include "Common/UdpSocketBuilder.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

namespace
{
	TAutoConsoleVariable<int32> CVarFakeLatencyMs(TEXT("apexsim.net.FakeLatencyMs"), 0,
		TEXT("Artificial round-trip latency on the UDP path, ms (0 off): half held on each input before it is sent, ")
		TEXT("half on each datagram received before it is decoded, so a ping can be felt on localhost. ")
		TEXT("Command line: -ApexFakeLatencyMs=N."));
}

int32 FApexUdpConnection::GetFakeLatencyMs()
{
	return FMath::Clamp(CVarFakeLatencyMs.GetValueOnAnyThread(), 0, 2000);
}

FApexUdpConnection::FApexUdpConnection(
	const FString& InHost, int32 InUdpPort, const FString& InUdpToken, const FString& InUdpKey)
	: Host(InHost)
	, UdpPort(InUdpPort)
	, UdpToken(InUdpToken)
	, UdpKey(InUdpKey)
{
	ReceiveBuffer.SetNumUninitialized(MaxDatagramBytes);
	SentInputs.SetNum(SentInputRing);
}

FApexUdpConnection::~FApexUdpConnection()
{
	Shutdown();
}

bool FApexUdpConnection::Start()
{
	Thread = FRunnableThread::Create(this, TEXT("ApexSimUdp"), 128 * 1024, TPri_AboveNormal);
	if (!Thread)
	{
		UE_LOG(LogApexSimNet, Error, TEXT("Failed to create the UDP thread"));
		return false;
	}
	return true;
}

void FApexUdpConnection::Shutdown()
{
	if (Thread)
	{
		Thread->Kill(/*bShouldWait=*/true);
		delete Thread;
		Thread = nullptr;
	}
	DestroySocket();
}

void FApexUdpConnection::SetPlayerInput(const FApexPlayerInput& Input)
{
	FScopeLock Lock(&InputLock);
	PendingInput = Input;
}

void FApexUdpConnection::CopySentInputs(uint32 AfterSeq, TArray<FApexSentInput>& Out) const
{
	Out.Reset();
	FScopeLock Lock(&SentInputLock);
	// Oldest first: the slot after the newest is the oldest the ring holds.
	for (int32 i = 0; i < SentInputRing; ++i)
	{
		const FApexSentInput& Sent = SentInputs[(SentInputHead + i) % SentInputRing];
		if (Sent.Seq != 0 && Sent.Seq > AfterSeq)
		{
			Out.Add(Sent);
		}
	}
}

bool FApexUdpConnection::PopTelemetry(FApexTelemetryFrame& OutFrame)
{
	return TelemetryQueue.Dequeue(OutFrame);
}

int32 FApexUdpConnection::DiscardQueuedTelemetry()
{
	int32 Discarded = 0;
	FApexTelemetryFrame Frame;
	while (TelemetryQueue.Dequeue(Frame))
	{
		++Discarded;
	}
	return Discarded;
}

bool FApexUdpConnection::PopSpectatorRecord(TArray<uint8>& OutRecords)
{
	return SpectatorQueue.Dequeue(OutRecords);
}

int32 FApexUdpConnection::DiscardQueuedSpectatorRecords()
{
	int32 Count = 0;
	TArray<uint8> Dropped;
	while (SpectatorQueue.Dequeue(Dropped))
	{
		++Count;
	}
	return Count;
}

bool FApexUdpConnection::PopDriverFeedback(FApexDriverFeedback& OutFeedback)
{
	return DriverFeedbackQueue.Dequeue(OutFeedback);
}

bool FApexUdpConnection::Init()
{
	return true;
}

void FApexUdpConnection::Stop()
{
	bStopRequested = true;
}

void FApexUdpConnection::Exit()
{
	DestroySocket();
}

void FApexUdpConnection::DestroySocket()
{
	if (Socket)
	{
		if (!SocketSubsystem)
		{
			SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		}
		if (SocketSubsystem)
		{
			SocketSubsystem->DestroySocket(Socket);
		}
		Socket = nullptr;
	}
}

bool FApexUdpConnection::CreateSocket(FString& OutError)
{
	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		OutError = TEXT("No socket subsystem available");
		return false;
	}

	const FAddressInfoResult AddressResult = SocketSubsystem->GetAddressInfo(
		*Host, nullptr, EAddressInfoFlags::Default, NAME_None, ESocketType::SOCKTYPE_Datagram);
	if (AddressResult.ReturnCode != SE_NO_ERROR || AddressResult.Results.Num() == 0)
	{
		OutError = FString::Printf(TEXT("Could not resolve UDP host '%s'"), *Host);
		return false;
	}

	ServerAddr = AddressResult.Results[0].Address->Clone();
	ServerAddr->SetPort(UdpPort);

	// Bound to port 0: the OS picks the source port, and the server learns it
	// from the handshake datagram's source address.
	Socket = FUdpSocketBuilder(TEXT("ApexSimUdp"))
		.AsReusable()
		.WithBroadcast()
		.BoundToPort(0)
		.WithReceiveBufferSize(256 * 1024)
		.WithSendBufferSize(64 * 1024)
		.Build();

	if (!Socket)
	{
		OutError = TEXT("Could not create a UDP socket");
		return false;
	}

	return true;
}

void FApexUdpConnection::SendSealed(TArrayView<const uint8> Payload)
{
	const TArray<uint8> Datagram = ApexProtocol::SealUdpDatagram(UdpKey, ++SendSeq, Payload);
	int32 Sent = 0;
	Socket->SendTo(Datagram.GetData(), Datagram.Num(), Sent, *ServerAddr);
}

void FApexUdpConnection::SendHandshake()
{
	const TArray<uint8> Payload = ApexProtocol::EncodeUdpHandshake(UdpToken);
	SendSealed(Payload);
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> UdpHandshake (%d bytes, seq %llu)"), Payload.Num(), SendSeq);
}

void FApexUdpConnection::SendPlayerInput()
{
	FApexPlayerInput Input;
	{
		FScopeLock Lock(&InputLock);
		Input = PendingInput;
	}

	// Every input is numbered, from 1, so the server can say which it had
	// applied when it took the own car's snapshot (client prediction). 0 is
	// "none" on the wire, so the count skips it if it ever wraps.
	if (++InputSeq == 0)
	{
		InputSeq = 1;
	}
	const TArray<uint8> Payload =
		ApexProtocol::EncodePlayerInput(static_cast<uint32>(LastServerTick.GetValue()), Input, InputSeq);
	const double Now = FPlatformTime::Seconds();
	const int32 FakeLatencyMs = GetFakeLatencyMs();
	if (FakeLatencyMs > 0 || DelayedOut.Num() > 0)
	{
		// Held for half the fake round trip; sealed when it goes, so the
		// seal's sequence still rises in the order the server sees.
		DelayedOut.Add({Now + FakeLatencyMs * 0.0005, Payload});
	}
	else
	{
		SendSealed(Payload);
	}
	{
		FScopeLock Lock(&SentInputLock);
		SentInputs[SentInputHead] = {InputSeq, Input, Now};
		SentInputHead = (SentInputHead + 1) % SentInputRing;
	}
	LastInputSeq.Set(static_cast<int32>(InputSeq));
}

void FApexUdpConnection::FlushDelayed(double Now)
{
	// In order: a later one is never due before an earlier one at a steady
	// latency, and when the latency is lowered the queue drains in order.
	int32 Sent = 0;
	while (Sent < DelayedOut.Num() && DelayedOut[Sent].DueSeconds <= Now)
	{
		SendSealed(DelayedOut[Sent].Bytes);
		++Sent;
	}
	DelayedOut.RemoveAt(0, Sent, EAllowShrinking::No);
	int32 Handled = 0;
	while (Handled < DelayedIn.Num() && DelayedIn[Handled].DueSeconds <= Now)
	{
		HandleDatagram(DelayedIn[Handled].Bytes);
		++Handled;
	}
	DelayedIn.RemoveAt(0, Handled, EAllowShrinking::No);
}

double FApexUdpConnection::SecondsToNextDelayed(double Now) const
{
	double Next = 1.0e9;
	if (DelayedOut.Num() > 0)
	{
		Next = FMath::Min(Next, DelayedOut[0].DueSeconds - Now);
	}
	if (DelayedIn.Num() > 0)
	{
		Next = FMath::Min(Next, DelayedIn[0].DueSeconds - Now);
	}
	return Next;
}

void FApexUdpConnection::ReceiveAvailable()
{
	uint32 PendingBytes = 0;
	while (Socket->HasPendingData(PendingBytes))
	{
		TSharedRef<FInternetAddr> From = SocketSubsystem->CreateInternetAddr();
		int32 BytesRead = 0;
		if (!Socket->RecvFrom(ReceiveBuffer.GetData(), ReceiveBuffer.Num(), BytesRead, *From) || BytesRead <= 0)
		{
			if (PendingBytes > 0 && !bLoggedReceiveFailure)
			{
				// A datagram larger than the buffer is discarded by the OS, and a
				// stream of them looks like the server went quiet.
				bLoggedReceiveFailure = true;
				UE_LOG(LogApexSimNet, Warning, TEXT("UDP receive failed with %u byte(s) pending (buffer %d)"),
					PendingBytes, ReceiveBuffer.Num());
			}
			return;
		}

		ReceivedDatagrams.Increment();

		const TArrayView<const uint8> Bytes(ReceiveBuffer.GetData(), BytesRead);
		const int32 FakeLatencyMs = GetFakeLatencyMs();
		if (FakeLatencyMs > 0 || DelayedIn.Num() > 0)
		{
			DelayedIn.Add({FPlatformTime::Seconds() + FakeLatencyMs * 0.0005, TArray<uint8>(Bytes)});
			continue;
		}
		HandleDatagram(Bytes);
	}
}

void FApexUdpConnection::HandleDatagram(TArrayView<const uint8> Bytes)
{
	const int32 BytesRead = Bytes.Num();
	FApexServerMessage Message;
	FString Error;
	if (!ApexProtocol::DecodeUdpMessage(Bytes, Message, Error))
	{
		// A bad datagram is not fatal — UDP is allowed to deliver rubbish —
		// but the first one is worth seeing: a steady stream of them freezes
		// every car without another word in the log.
		if (!bLoggedDecodeFailure)
		{
			bLoggedDecodeFailure = true;
			UE_LOG(LogApexSimNet, Warning, TEXT("Dropped an undecodable %d byte datagram: %s"), BytesRead, *Error);
		}
		else
		{
			UE_LOG(LogApexSimNet, Verbose, TEXT("Dropped a %d byte datagram: %s"), BytesRead, *Error);
		}
		return;
	}

	switch (Message.Type)
	{
	case EApexServerMessageType::UdpHandshakeAck:
		if (!bHandshakeComplete)
		{
			bHandshakeComplete = true;
			UE_LOG(LogApexSimNet, Log, TEXT("<- UdpHandshakeAck; telemetry now flows over UDP"));
		}
		break;

	case EApexServerMessageType::TelemetryCompact:
		// Telemetry arriving at all means the server has bound us, even if
		// the ack datagram itself was lost.
		if (!bHandshakeComplete)
		{
			bHandshakeComplete = true;
			UE_LOG(LogApexSimNet, Log, TEXT("Telemetry arrived before the ack; treating the handshake as done"));
		}
		LastServerTick.Set(static_cast<int32>(Message.Telemetry.ServerTick));
		TelemetryQueue.Enqueue(MoveTemp(Message.Telemetry));
		break;

	case EApexServerMessageType::DriverFeedback:
		Message.DriverFeedback.ReceivedSeconds = FPlatformTime::Seconds();
		DriverFeedbackQueue.Enqueue(MoveTemp(Message.DriverFeedback));
		break;

	case EApexServerMessageType::SpectatorRecord:
		// A showcase frame: the server bound us, as with telemetry.
		if (!bHandshakeComplete)
		{
			bHandshakeComplete = true;
			UE_LOG(LogApexSimNet, Log, TEXT("A stream frame arrived before the ack; treating the handshake as done"));
		}
		SpectatorQueue.Enqueue(MoveTemp(Message.SpectatorRecords));
		break;

	default:
		UE_LOG(LogApexSimNet, Verbose, TEXT("Ignoring UDP message '%s'"), *Message.VariantName);
		break;
	}
}

uint32 FApexUdpConnection::Run()
{
	FString Error;
	if (!CreateSocket(Error))
	{
		UE_LOG(LogApexSimNet, Error, TEXT("UDP setup failed: %s"), *Error);
		return 0;
	}

	UE_LOG(LogApexSimNet, Log, TEXT("UDP socket open, handshaking with %s:%d"), *Host, UdpPort);

	const double InputInterval = 1.0 / InputSendHz;
	double LastHandshake = 0.0;
	double LastInput = 0.0;

	while (!bStopRequested)
	{
		const double Now = FPlatformTime::Seconds();

		if (!bHandshakeComplete)
		{
			// Datagrams may be lost, so keep asking until the server answers.
			if (Now - LastHandshake >= HandshakeRetryMs / 1000.0)
			{
				LastHandshake = Now;
				SendHandshake();
			}
		}
		else if (Now - LastInput >= InputInterval)
		{
			LastInput = Now;
			SendPlayerInput();
		}

		// Under a fake latency the wait ends in time for the next datagram due.
		const double WaitMs = FMath::Clamp(SecondsToNextDelayed(FPlatformTime::Seconds()) * 1000.0, 0.0,
			static_cast<double>(PollIntervalMs));
		Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(WaitMs));
		if (bStopRequested)
		{
			break;
		}

		ReceiveAvailable();
		FlushDelayed(FPlatformTime::Seconds());
	}

	UE_LOG(LogApexSimNet, Log, TEXT("UDP thread stopping (%d datagrams received)"), ReceivedDatagrams.GetValue());
	return 0;
}
