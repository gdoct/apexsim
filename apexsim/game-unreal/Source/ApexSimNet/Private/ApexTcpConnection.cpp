#include "ApexTcpConnection.h"

#include "ApexProtocolCodec.h"
#include "ApexSimNetModule.h"
#include "ApexTlsSession.h"
#include "Common/TcpSocketBuilder.h"
#include "HAL/RunnableThread.h"
#include "Misc/ScopeLock.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

FApexTcpConnection::FApexTcpConnection(
	const FString& InHost,
	int32 InPort,
	const FString& InToken,
	const FString& InPlayerName,
	const FApexTlsOptions& InTls,
	const FString& InResumeToken)
	: Host(InHost)
	, Port(InPort)
	, Token(InToken)
	, PlayerName(InPlayerName)
	, TlsOptions(InTls)
	, ResumeToken(InResumeToken)
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

void FApexTcpConnection::SetDisconnectReason(const FString& Text, bool bDuringConnect, bool bPermanent)
{
	if (bDisconnectReported)
	{
		return;
	}
	bDisconnectReported = true;
	DisconnectQueue.Enqueue(FApexDisconnectReason{Text, bDuringConnect, bPermanent});
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
	FScopeLock Lock(&SocketLock);
	if (Socket)
	{
		Socket->Shutdown(ESocketShutdownMode::ReadWrite);
		Socket->Close();
	}
}

void FApexTcpConnection::Exit()
{
	if (Tls)
	{
		// Best effort: a close_notify tells the server this was not a truncation.
		Tls->Close();
		FlushCiphertext();
		Tls.Reset();
	}
	bEncrypted = false;
	DestroySocket();
	bConnected = false;
}

void FApexTcpConnection::DestroySocket()
{
	FScopeLock Lock(&SocketLock);
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

	FSocket* NewSocket = FTcpSocketBuilder(TEXT("ApexSimTcp"))
		.AsBlocking()
		.WithReceiveBufferSize(ReceiveBufferBytes)
		.WithSendBufferSize(SendBufferBytes)
		.Build();
	if (!NewSocket)
	{
		OutError = TEXT("Could not create a TCP socket");
		return false;
	}
	{
		FScopeLock Lock(&SocketLock);
		Socket = NewSocket;
	}
	if (bStopRequested)
	{
		// Stopped while the socket was being made: Stop() had nothing to
		// close, so do not sit in Connect().
		OutError = TEXT("Stopped before connecting");
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

bool FApexTcpConnection::SendRaw(const uint8* Bytes, int32 Num)
{
	int32 TotalSent = 0;
	while (TotalSent < Num)
	{
		if (bStopRequested || !Socket)
		{
			return false;
		}
		int32 Sent = 0;
		if (!Socket->Send(Bytes + TotalSent, Num - TotalSent, Sent) || Sent <= 0)
		{
			return false;
		}
		TotalSent += Sent;
	}
	return true;
}

bool FApexTcpConnection::FlushCiphertext()
{
	if (!Tls)
	{
		return true;
	}
	TArray<uint8> Ciphertext;
	if (!Tls->TakeCiphertext(Ciphertext))
	{
		return true;
	}
	return SendRaw(Ciphertext.GetData(), Ciphertext.Num());
}

bool FApexTcpConnection::SendAll(const TArray<uint8>& Bytes)
{
	if (!Tls)
	{
		return SendRaw(Bytes.GetData(), Bytes.Num());
	}
	FString Error;
	if (!Tls->Write(Bytes.GetData(), Bytes.Num(), Error))
	{
		UE_LOG(LogApexSimNet, Warning, TEXT("TLS write failed: %s"), *Error);
		return false;
	}
	return FlushCiphertext();
}

bool FApexTcpConnection::RecvRaw(TArray<uint8>& Out, FString& OutError)
{
	// The socket is blocking, but Wait said it is readable, so this Recv
	// returns at once: with bytes, or with nothing because the peer closed or
	// reset the connection. HasPendingData alone cannot tell a closed
	// connection from an idle one.
	constexpr int32 ChunkBytes = 64 * 1024;
	int32 Offset = Out.Num();
	Out.AddUninitialized(ChunkBytes);
	int32 BytesRead = 0;
	if (!Socket->Recv(Out.GetData() + Offset, ChunkBytes, BytesRead) || BytesRead <= 0)
	{
		Out.SetNum(Offset, EAllowShrinking::No);
		OutError = TEXT("Server closed the connection");
		return false;
	}
	Out.SetNum(Offset + BytesRead, EAllowShrinking::No);

	uint32 PendingBytes = 0;
	while (Socket->HasPendingData(PendingBytes) && PendingBytes > 0)
	{
		const int32 ChunkSize = static_cast<int32>(FMath::Min<uint32>(PendingBytes, static_cast<uint32>(ChunkBytes)));
		Offset = Out.Num();
		Out.AddUninitialized(ChunkSize);
		BytesRead = 0;
		if (!Socket->Recv(Out.GetData() + Offset, ChunkSize, BytesRead) || BytesRead <= 0)
		{
			Out.SetNum(Offset, EAllowShrinking::No);
			OutError = TEXT("Connection lost");
			return false;
		}
		Out.SetNum(Offset + BytesRead, EAllowShrinking::No);
	}
	return true;
}

bool FApexTcpConnection::HandshakeTls(FString& OutError, EApexTlsFailure& OutFailure)
{
	OutFailure = EApexTlsFailure::None;
	Tls = MakeUnique<FApexTlsSession>();
	FString Error;
	if (!Tls->Init(Host, TlsOptions, Error))
	{
		OutFailure = EApexTlsFailure::Setup;
		OutError = Error;
		return false;
	}

	const double Deadline = FPlatformTime::Seconds() + TlsHandshakeTimeoutSeconds;
	int64 BytesReceived = 0;
	uint8 FirstByte = 0;
	TArray<uint8> Raw;

	for (;;)
	{
		const FApexTlsSession::EStep Step = Tls->StepHandshake(Error);
		const bool bSent = FlushCiphertext();
		if (Step == FApexTlsSession::EStep::Done)
		{
			break;
		}
		if (Step == FApexTlsSession::EStep::Failed)
		{
			OutFailure = ApexTls::ClassifyFailure(BytesReceived, FirstByte, EApexTlsFailure::Handshake);
			OutError = OutFailure == EApexTlsFailure::Handshake
				? FString::Printf(TEXT("TLS handshake with %s:%d failed: %s"), *Host, Port, *Error)
				: FString::Printf(TEXT("%s:%d did not answer in TLS (%s)"), *Host, Port, *Error);
			return false;
		}
		if (!bSent)
		{
			OutFailure = ApexTls::ClassifyFailure(BytesReceived, FirstByte, EApexTlsFailure::Handshake);
			OutError = FString::Printf(TEXT("Connection to %s:%d lost during the TLS handshake"), *Host, Port);
			return false;
		}

		// Wait for the server's next flight.
		bool bReadable = false;
		while (!bReadable)
		{
			if (bStopRequested)
			{
				OutError = TEXT("Stopped during the TLS handshake");
				OutFailure = EApexTlsFailure::Handshake;
				return false;
			}
			if (FPlatformTime::Seconds() > Deadline)
			{
				OutFailure = EApexTlsFailure::Timeout;
				OutError = FString::Printf(TEXT("%s:%d did not complete a TLS handshake in %.0f s"),
					*Host, Port, TlsHandshakeTimeoutSeconds);
				return false;
			}
			bReadable = Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(PollIntervalMs));
		}

		Raw.Reset();
		FString RecvError;
		const bool bOpen = RecvRaw(Raw, RecvError);
		if (Raw.Num() > 0)
		{
			if (BytesReceived == 0)
			{
				FirstByte = Raw[0];
			}
			BytesReceived += Raw.Num();
			Tls->FeedCiphertext(Raw.GetData(), Raw.Num());
		}
		if (!bOpen)
		{
			// Let OpenSSL read what did arrive (an alert, say) before giving up.
			if (Raw.Num() > 0 && Tls->StepHandshake(Error) == FApexTlsSession::EStep::Done)
			{
				break;
			}
			OutFailure = ApexTls::ClassifyFailure(BytesReceived, FirstByte, EApexTlsFailure::Handshake);
			OutError = OutFailure == EApexTlsFailure::ClosedBeforeReply
				? FString::Printf(TEXT("%s:%d closed the connection on the TLS hello"), *Host, Port)
				: FString::Printf(TEXT("TLS handshake with %s:%d failed: %s"), *Host, Port,
					Error.IsEmpty() ? *RecvError : *Error);
			return false;
		}
	}

	if (!Tls->CheckPeer(Error))
	{
		OutFailure = EApexTlsFailure::Untrusted;
		OutError = FString::Printf(TEXT("Refused %s:%d: %s"), *Host, Port, *Error);
		Tls->Close();
		FlushCiphertext();
		return false;
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
	if (!Tls)
	{
		return RecvRaw(ReceiveBuffer, OutError);
	}

	TArray<uint8> Raw;
	FString RecvError;
	const bool bOpen = RecvRaw(Raw, RecvError);
	Tls->FeedCiphertext(Raw.GetData(), Raw.Num());

	// Decrypt what arrived even when the socket then closed: the last frames
	// before a server's close are still worth having.
	FString TlsError;
	const FApexTlsSession::ERead Read = Tls->ReadAvailable(ReceiveBuffer, TlsError);
	// Reading can make the session answer (a TLS 1.3 key update).
	FlushCiphertext();
	if (Read == FApexTlsSession::ERead::Closed)
	{
		OutError = TEXT("Server closed the connection");
		return false;
	}
	if (Read == FApexTlsSession::ERead::Failed)
	{
		OutError = FString::Printf(TEXT("TLS error: %s"), *TlsError);
		return false;
	}
	if (!bOpen)
	{
		OutError = RecvError;
		return false;
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

	bool bTryTls = TlsOptions.Mode != EApexTlsMode::Off;
	for (;;)
	{
		if (!ConnectSocket(Error))
		{
			UE_LOG(LogApexSimNet, Warning, TEXT("Connect failed: %s"), *Error);
			SetDisconnectReason(Error, true);
			return 0;
		}
		if (!bTryTls)
		{
			break;
		}

		EApexTlsFailure Failure = EApexTlsFailure::None;
		if (HandshakeTls(Error, Failure))
		{
			bEncrypted = true;
			break;
		}
		Tls.Reset();
		if (bStopRequested)
		{
			SetDisconnectReason(TEXT("Disconnected"), true);
			return 0;
		}
		if (ApexTls::ShouldFallBackToPlaintext(TlsOptions, Failure))
		{
			// The server is a plaintext one (it dropped the hello unanswered, or
			// answered in something that is not TLS): connect again without.
			UE_LOG(LogApexSimNet, Warning,
				TEXT("%s; the server does not speak TLS, reconnecting in plaintext (settings.yml server.tls: auto)"),
				*Error);
			DestroySocket();
			bTryTls = false;
			continue;
		}

		const bool bPermanent = ApexTls::IsPermanent(Failure);
		UE_LOG(LogApexSimNet, Warning, TEXT("%s"), *Error);
		if (TlsOptions.Mode == EApexTlsMode::On
			&& (Failure == EApexTlsFailure::ClosedBeforeReply || Failure == EApexTlsFailure::NotTls))
		{
			UE_LOG(LogApexSimNet, Warning,
				TEXT("settings.yml has server.tls: on, so there is no plaintext fallback; the server may not have TLS enabled"));
		}
		SetDisconnectReason(Error, true, bPermanent);
		return 0;
	}

	bConnected = true;
	if (Tls)
	{
		UE_LOG(LogApexSimNet, Log, TEXT("Connected to %s:%d over TLS (%s; %s; SHA-256 %s; server.tls: %s)"),
			*Host, Port, *Tls->DescribeCipher(), Tls->TrustDescription(), *Tls->PeerFingerprint(),
			ApexTls::ModeName(TlsOptions.Mode));
	}
	else
	{
		UE_LOG(LogApexSimNet, Log, TEXT("Connected to %s:%d in PLAINTEXT (unencrypted; server.tls: %s)"),
			*Host, Port, ApexTls::ModeName(TlsOptions.Mode));
	}

	// Authenticate is sent from this thread the instant the socket is up, so
	// the handshake never waits on a game-thread tick.
	TArray<uint8> AuthPayload = ApexProtocol::EncodeAuthenticate(Token, PlayerName, ResumeToken);
	UE_LOG(LogApexSimNet, Verbose, TEXT("-> Authenticate (protocol_version=%d, resume=%d, %d bytes)"),
		APEXSIM_PROTOCOL_VERSION, ResumeToken.IsEmpty() ? 0 : 1, AuthPayload.Num());
	OutboundQueue.Enqueue(MoveTemp(AuthPayload));

	while (!bStopRequested)
	{
		if (!FlushOutbound())
		{
			Error = TEXT("Connection lost while sending");
			break;
		}

		const bool bReadable = Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(PollIntervalMs));

		if (bStopRequested)
		{
			break;
		}
		if (!bReadable)
		{
			continue;
		}

		// Frames that arrived just ahead of a close are still delivered.
		const bool bOpen = ReceiveAvailable(Error);
		FString FrameError;
		if (!ExtractFrames(FrameError))
		{
			Error = FrameError;
			break;
		}
		if (!bOpen)
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
