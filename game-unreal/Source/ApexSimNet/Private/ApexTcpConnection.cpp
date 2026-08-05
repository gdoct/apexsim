#include "ApexTcpConnection.h"

#include "ApexProtocolCodec.h"
#include "ApexSimNetModule.h"
#include "Common/TcpSocketBuilder.h"
#include "HAL/RunnableThread.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

FApexTcpConnection::FApexTcpConnection(
	const FString& InHost,
	int32 InPort,
	const FString& InToken,
	const FString& InPlayerName)
	: Host(InHost)
	, Port(InPort)
	, Token(InToken)
	, PlayerName(InPlayerName)
{
}

FApexTcpConnection::~FApexTcpConnection()
{
	Shutdown();
}

bool FApexTcpConnection::Start()
{
	// TPri_BelowNormal: this thread is latency-tolerant (a 50 ms poll) and must
	// never compete with the game thread.
	Thread = FRunnableThread::Create(this, TEXT("ApexSimNet"), 128 * 1024, TPri_BelowNormal);
	if (!Thread)
	{
		UE_LOG(LogApexSimNet, Error, TEXT("Failed to create the network thread"));
		SetDisconnectReason(TEXT("Failed to create the network thread"), true);
		return false;
	}
	return true;
}

void FApexTcpConnection::Shutdown()
{
	if (Thread)
	{
		// Kill(true) runs Stop() then joins. Stop() breaks the blocking Wait()
		// by shutting the socket down, so this cannot hang for a poll interval.
		Thread->Kill(true);
		delete Thread;
		Thread = nullptr;
	}

	// Normally Exit() already did this on the worker thread; this covers the
	// case where the thread never started.
	DestroySocket();
}

void FApexTcpConnection::Send(TArray<uint8>&& Payload)
{
	if (bStopRequested)
	{
		return;
	}
	OutboundQueue.Enqueue(MoveTemp(Payload));
}

bool FApexTcpConnection::PopMessage(FApexServerMessage& OutMessage)
{
	return InboundQueue.Dequeue(OutMessage);
}

bool FApexTcpConnection::PopDisconnectReason(FApexDisconnectReason& OutReason)
{
	return DisconnectQueue.Dequeue(OutReason);
}

void FApexTcpConnection::SetDisconnectReason(const FString& Text, bool bDuringConnect)
{
	if (bDisconnectReported)
	{
		return;
	}
	bDisconnectReported = true;
	DisconnectQueue.Enqueue(FApexDisconnectReason{Text, bDuringConnect});
}

bool FApexTcpConnection::Init()
{
	// Deliberately empty: connecting here would block whoever created the
	// thread. Everything happens in Run().
	return true;
}

void FApexTcpConnection::Stop()
{
	bStopRequested = true;

	// Break the worker out of Socket->Wait() immediately rather than waiting
	// for the poll to expire. Shutdown/Close are safe to call cross-thread;
	// destroying the socket is not, so that stays in Exit().
	if (Socket)
	{
		Socket->Shutdown(ESocketShutdownMode::ReadWrite);
		Socket->Close();
	}
}

void FApexTcpConnection::Exit()
{
	DestroySocket();
	bConnected = false;
}

void FApexTcpConnection::DestroySocket()
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

bool FApexTcpConnection::ConnectSocket(FString& OutError)
{
	SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		OutError = TEXT("No socket subsystem available");
		return false;
	}

	// GetAddressInfo handles both a literal address and a hostname, so the
	// connect dialog accepts either.
	const FAddressInfoResult AddressResult = SocketSubsystem->GetAddressInfo(
		*Host,
		nullptr,
		EAddressInfoFlags::Default,
		NAME_None,
		ESocketType::SOCKTYPE_Streaming);

	if (AddressResult.ReturnCode != SE_NO_ERROR || AddressResult.Results.Num() == 0)
	{
		OutError = FString::Printf(TEXT("Could not resolve host '%s'"), *Host);
		return false;
	}

	TSharedRef<FInternetAddr> Address = AddressResult.Results[0].Address->Clone();
	Address->SetPort(Port);

	Socket = FTcpSocketBuilder(TEXT("ApexSimTcp"))
		.AsBlocking()
		.WithReceiveBufferSize(ReceiveBufferBytes)
		.WithSendBufferSize(SendBufferBytes)
		.Build();

	if (!Socket)
	{
		OutError = TEXT("Could not create a TCP socket");
		return false;
	}

	if (!Socket->Connect(*Address))
	{
		OutError = FString::Printf(TEXT("Could not connect to %s:%d"), *Host, Port);
		return false;
	}

	// Menu traffic is small and latency-sensitive; Nagle would coalesce an
	// Authenticate with whatever follows it.
	Socket->SetNoDelay(true);
	return true;
}

bool FApexTcpConnection::SendAll(const TArray<uint8>& Bytes)
{
	int32 TotalSent = 0;
	while (TotalSent < Bytes.Num())
	{
		if (bStopRequested)
		{
			return false;
		}
		int32 Sent = 0;
		if (!Socket->Send(Bytes.GetData() + TotalSent, Bytes.Num() - TotalSent, Sent) || Sent <= 0)
		{
			return false;
		}
		TotalSent += Sent;
	}
	return true;
}

bool FApexTcpConnection::FlushOutbound()
{
	TArray<uint8> Payload;
	while (OutboundQueue.Dequeue(Payload))
	{
		// [4-byte big-endian length][payload], matching transport.rs:515-517.
		const uint32 Length = static_cast<uint32>(Payload.Num());
		TArray<uint8> Frame;
		Frame.Reserve(4 + Payload.Num());
		Frame.Add(static_cast<uint8>((Length >> 24) & 0xFF));
		Frame.Add(static_cast<uint8>((Length >> 16) & 0xFF));
		Frame.Add(static_cast<uint8>((Length >> 8) & 0xFF));
		Frame.Add(static_cast<uint8>(Length & 0xFF));
		Frame.Append(Payload);

		if (!SendAll(Frame))
		{
			return false;
		}
	}
	return true;
}

bool FApexTcpConnection::ReceiveAvailable(FString& OutError)
{
	uint32 PendingBytes = 0;
	while (Socket->HasPendingData(PendingBytes) && PendingBytes > 0)
	{
		const int32 ChunkSize = static_cast<int32>(FMath::Min<uint32>(PendingBytes, 64u * 1024u));
		const int32 WriteOffset = ReceiveBuffer.Num();
		ReceiveBuffer.AddUninitialized(ChunkSize);

		int32 BytesRead = 0;
		if (!Socket->Recv(ReceiveBuffer.GetData() + WriteOffset, ChunkSize, BytesRead))
		{
			ReceiveBuffer.SetNum(WriteOffset, EAllowShrinking::No);
			OutError = TEXT("Connection lost");
			return false;
		}

		if (BytesRead == 0)
		{
			// A clean close from the server side.
			ReceiveBuffer.SetNum(WriteOffset, EAllowShrinking::No);
			OutError = TEXT("Server closed the connection");
			return false;
		}

		ReceiveBuffer.SetNum(WriteOffset + BytesRead, EAllowShrinking::No);
	}
	return true;
}

bool FApexTcpConnection::ExtractFrames(FString& OutError)
{
	int32 Consumed = 0;

	// A single Recv can hold several frames, or half of one. Loop until the
	// buffer holds less than a complete frame.
	while (ReceiveBuffer.Num() - Consumed >= 4)
	{
		const uint8* Cursor = ReceiveBuffer.GetData() + Consumed;
		const uint32 FrameLength =
			(static_cast<uint32>(Cursor[0]) << 24) |
			(static_cast<uint32>(Cursor[1]) << 16) |
			(static_cast<uint32>(Cursor[2]) << 8) |
			 static_cast<uint32>(Cursor[3]);

		if (FrameLength > MaxFrameBytes)
		{
			OutError = FString::Printf(TEXT("Frame of %u bytes exceeds the %u byte limit"), FrameLength, MaxFrameBytes);
			return false;
		}

		const int64 TotalFrameSize = static_cast<int64>(FrameLength) + 4;
		if (ReceiveBuffer.Num() - Consumed < TotalFrameSize)
		{
			// Partial frame: wait for more bytes.
			break;
		}

		TArrayView<const uint8> Payload(Cursor + 4, static_cast<int32>(FrameLength));

		FApexServerMessage Message;
		FString DecodeError;
		if (ApexProtocol::DecodeServerMessage(Payload, Message, DecodeError))
		{
			if (Message.Type != EApexServerMessageType::IgnoredVariant)
			{
				InboundQueue.Enqueue(MoveTemp(Message));
			}
		}
		else
		{
			// A decode failure is a bug on our side, not a reason to drop the
			// connection — log it and keep the stream in sync.
			UE_LOG(LogApexSimNet, Warning, TEXT("Failed to decode a %u byte frame: %s"), FrameLength, *DecodeError);
		}

		Consumed += static_cast<int32>(TotalFrameSize);
	}

	if (Consumed > 0)
	{
		ReceiveBuffer.RemoveAt(0, Consumed, EAllowShrinking::No);
	}
	return true;
}

uint32 FApexTcpConnection::Run()
{
	FString Error;

	UE_LOG(LogApexSimNet, Log, TEXT("Connecting to %s:%d as '%s'"), *Host, Port, *PlayerName);

	if (!ConnectSocket(Error))
	{
		UE_LOG(LogApexSimNet, Warning, TEXT("Connect failed: %s"), *Error);
		SetDisconnectReason(Error, true);
		return 0;
	}

	bConnected = true;
	UE_LOG(LogApexSimNet, Log, TEXT("TCP connected to %s:%d"), *Host, Port);

	// Authenticate is sent from this thread the instant the socket is up, so
	// the handshake never waits on a game-thread tick.
	TArray<uint8> AuthPayload = ApexProtocol::EncodeAuthenticate(Token, PlayerName);
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> Authenticate (protocol_version=%d, %d bytes)"),
		APEXSIM_PROTOCOL_VERSION, AuthPayload.Num());
	OutboundQueue.Enqueue(MoveTemp(AuthPayload));

	while (!bStopRequested)
	{
		if (!FlushOutbound())
		{
			Error = TEXT("Connection lost while sending");
			break;
		}

		Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(PollIntervalMs));

		if (bStopRequested)
		{
			break;
		}

		if (!ReceiveAvailable(Error))
		{
			break;
		}

		if (!ExtractFrames(Error))
		{
			break;
		}
	}

	bConnected = false;

	if (bStopRequested)
	{
		SetDisconnectReason(TEXT("Disconnected"), false);
	}
	else
	{
		UE_LOG(LogApexSimNet, Warning, TEXT("Network thread stopping: %s"), *Error);
		SetDisconnectReason(Error.IsEmpty() ? TEXT("Connection lost") : Error, false);
	}

	return 0;
}
