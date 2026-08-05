#include "ApexUdpConnection.h"

#include "ApexProtocolCodec.h"
#include "ApexSimNetModule.h"
#include "Common/UdpSocketBuilder.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

FApexUdpConnection::FApexUdpConnection(const FString& InHost, int32 InUdpPort, const FString& InUdpToken)
	: Host(InHost)
	, UdpPort(InUdpPort)
	, UdpToken(InUdpToken)
{
	ReceiveBuffer.SetNumUninitialized(MaxDatagramBytes);
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

bool FApexUdpConnection::PopTelemetry(FApexTelemetryFrame& OutFrame)
{
	return TelemetryQueue.Dequeue(OutFrame);
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

void FApexUdpConnection::SendHandshake()
{
	const TArray<uint8> Payload = ApexProtocol::EncodeUdpHandshake(UdpToken);
	int32 Sent = 0;
	Socket->SendTo(Payload.GetData(), Payload.Num(), Sent, *ServerAddr);
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> UdpHandshake (%d bytes)"), Payload.Num());
}

void FApexUdpConnection::SendPlayerInput()
{
	FApexPlayerInput Input;
	{
		FScopeLock Lock(&InputLock);
		Input = PendingInput;
	}

	const TArray<uint8> Payload =
		ApexProtocol::EncodePlayerInput(static_cast<uint32>(LastServerTick.GetValue()), Input);
	int32 Sent = 0;
	Socket->SendTo(Payload.GetData(), Payload.Num(), Sent, *ServerAddr);
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
			return;
		}

		ReceivedDatagrams.Increment();

		FApexServerMessage Message;
		FString Error;
		if (!ApexProtocol::DecodeUdpMessage(
				TArrayView<const uint8>(ReceiveBuffer.GetData(), BytesRead), Message, Error))
		{
			// A bad datagram is not fatal — UDP is allowed to deliver rubbish.
			UE_LOG(LogApexSimNet, Verbose, TEXT("Dropped a %d byte datagram: %s"), BytesRead, *Error);
			continue;
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

		default:
			UE_LOG(LogApexSimNet, Verbose, TEXT("Ignoring UDP message '%s'"), *Message.VariantName);
			break;
		}
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

		Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(PollIntervalMs));
		if (bStopRequested)
		{
			break;
		}

		ReceiveAvailable();
	}

	UE_LOG(LogApexSimNet, Log, TEXT("UDP thread stopping (%d datagrams received)"), ReceivedDatagrams.GetValue());
	return 0;
}
