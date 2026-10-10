#include "ApexTlsSession.h"

#include "ApexSimNetModule.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
THIRD_PARTY_INCLUDES_START
// OpenSSL's UI typedef clashes with Unreal's UI namespace (as in the
// engine's BuildPatchServices Crypto.cpp).
#define UI UI_ST
// wincrypt.h first: OpenSSL's headers #undef the names wincrypt.h defines
// (X509_NAME...), not the other way round.
#include <wincrypt.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/evp.h>
#undef UI
THIRD_PARTY_INCLUDES_END
#include "Windows/HideWindowsPlatformTypes.h"
#else
THIRD_PARTY_INCLUDES_START
// OpenSSL's UI typedef clashes with Unreal's UI namespace (as in the
// engine's BuildPatchServices Crypto.cpp).
#define UI UI_ST
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/evp.h>
#undef UI
THIRD_PARTY_INCLUDES_END
#endif

namespace ApexTlsSessionPrivate
{
	/** The OpenSSL error queue as one line, and empties it. */
	FString DrainErrorQueue()
	{
		FString Text;
		unsigned long Code = 0;
		while ((Code = ERR_get_error()) != 0)
		{
			char Buffer[256];
			ERR_error_string_n(Code, Buffer, sizeof(Buffer));
			if (!Text.IsEmpty())
			{
				Text += TEXT("; ");
			}
			Text += UTF8_TO_TCHAR(Buffer);
		}
		return Text;
	}

	/** The system's trusted roots into the context's store. Returns how many. */
	int32 AddSystemRoots(SSL_CTX* Context)
	{
#if PLATFORM_WINDOWS
		X509_STORE* Store = SSL_CTX_get_cert_store(Context);
		HCERTSTORE SystemRoots = CertOpenSystemStoreW(0, L"ROOT");
		if (!SystemRoots)
		{
			return 0;
		}
		int32 Added = 0;
		PCCERT_CONTEXT Cert = nullptr;
		while ((Cert = CertEnumCertificatesInStore(SystemRoots, Cert)) != nullptr)
		{
			if ((Cert->dwCertEncodingType & X509_ASN_ENCODING) == 0)
			{
				continue;
			}
			const unsigned char* Der = Cert->pbCertEncoded;
			if (X509* Parsed = d2i_X509(nullptr, &Der, static_cast<long>(Cert->cbCertEncoded)))
			{
				// A duplicate is an error OpenSSL queues; it is harmless.
				if (X509_STORE_add_cert(Store, Parsed) == 1)
				{
					++Added;
				}
				X509_free(Parsed);
			}
		}
		CertCloseStore(SystemRoots, 0);
		ERR_clear_error();
		return Added;
#else
		return SSL_CTX_set_default_verify_paths(Context) == 1 ? 1 : 0;
#endif
	}
}

FApexTlsSession::~FApexTlsSession()
{
	if (Ssl)
	{
		SSL_free(Ssl);  // frees both BIOs
		Ssl = nullptr;
	}
	if (Context)
	{
		SSL_CTX_free(Context);
		Context = nullptr;
	}
	ERR_clear_error();
}

bool FApexTlsSession::Init(const FString& InHost, const FApexTlsOptions& InOptions, FString& OutError)
{
	Host = InHost;
	Options = InOptions;

	Context = SSL_CTX_new(TLS_client_method());
	if (!Context)
	{
		OutError = FString::Printf(TEXT("Could not set up TLS: %s"), *ApexTlsSessionPrivate::DrainErrorQueue());
		return false;
	}
	SSL_CTX_set_min_proto_version(Context, TLS1_2_VERSION);
	SSL_CTX_set_options(Context, SSL_OP_NO_COMPRESSION);
	// The check is made in CheckPeer, after the handshake, so a refusal can say
	// which certificate it was and how to trust it. Nothing is sent before it.
	SSL_CTX_set_verify(Context, SSL_VERIFY_NONE, nullptr);

	const bool bChain = Options.bVerify && !Options.IsPinned();
	if (bChain)
	{
		const int32 Roots = ApexTlsSessionPrivate::AddSystemRoots(Context);
		UE_LOG(LogApexSimNet, Verbose, TEXT("TLS: %d trusted root certificate(s) loaded"), Roots);
	}

	Ssl = SSL_new(Context);
	if (!Ssl)
	{
		OutError = FString::Printf(TEXT("Could not set up TLS: %s"), *ApexTlsSessionPrivate::DrainErrorQueue());
		return false;
	}

	ReadBio = BIO_new(BIO_s_mem());
	WriteBio = BIO_new(BIO_s_mem());
	if (!ReadBio || !WriteBio)
	{
		if (ReadBio) { BIO_free(ReadBio); ReadBio = nullptr; }
		if (WriteBio) { BIO_free(WriteBio); WriteBio = nullptr; }
		OutError = TEXT("Could not set up TLS buffers");
		return false;
	}
	// An empty read BIO means "wait for more", not end of file.
	BIO_set_mem_eof_return(ReadBio, -1);
	SSL_set_bio(Ssl, ReadBio, WriteBio);
	SSL_set_connect_state(Ssl);

	const FTCHARToUTF8 HostUtf8(*Host);
	X509_VERIFY_PARAM* Param = SSL_get0_param(Ssl);
	// An address is checked against the certificate's IP entries and gets no
	// SNI (RFC 6066 forbids one); a name gets both.
	const bool bAddress = X509_VERIFY_PARAM_set1_ip_asc(Param, HostUtf8.Get()) == 1;
	ERR_clear_error();
	if (!bAddress)
	{
		SSL_set_tlsext_host_name(Ssl, const_cast<char*>(HostUtf8.Get()));
		if (bChain)
		{
			X509_VERIFY_PARAM_set_hostflags(Param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
			X509_VERIFY_PARAM_set1_host(Param, HostUtf8.Get(), 0);
		}
	}
	return true;
}

FString FApexTlsSession::ErrorString(int32 SslResult) const
{
	const int32 Code = SSL_get_error(Ssl, SslResult);
	const FString Queue = ApexTlsSessionPrivate::DrainErrorQueue();
	switch (Code)
	{
	case SSL_ERROR_ZERO_RETURN: return TEXT("the server closed the TLS session");
	case SSL_ERROR_SYSCALL:     return Queue.IsEmpty() ? FString(TEXT("the connection ended mid-record")) : Queue;
	default:                    return Queue.IsEmpty() ? FString::Printf(TEXT("TLS error %d"), Code) : Queue;
	}
}

FApexTlsSession::EStep FApexTlsSession::StepHandshake(FString& OutError)
{
	const int Result = SSL_do_handshake(Ssl);
	if (Result == 1)
	{
		if (X509* Peer = SSL_get_peer_certificate(Ssl))
		{
			uint8 Digest[EVP_MAX_MD_SIZE];
			unsigned int Length = 0;
			if (X509_digest(Peer, EVP_sha256(), Digest, &Length) == 1)
			{
				Fingerprint = ApexTls::FormatFingerprint(TArrayView<const uint8>(Digest, static_cast<int32>(Length)));
			}
			X509_free(Peer);
		}
		return EStep::Done;
	}
	const int Code = SSL_get_error(Ssl, Result);
	if (Code == SSL_ERROR_WANT_READ || Code == SSL_ERROR_WANT_WRITE)
	{
		return EStep::WantRead;
	}
	OutError = ErrorString(Result);
	return EStep::Failed;
}

bool FApexTlsSession::CheckPeer(FString& OutError)
{
	if (Fingerprint.IsEmpty())
	{
		OutError = TEXT("the server presented no certificate");
		return false;
	}

	if (Options.IsPinned())
	{
		FString Want;
		FString Have;
		ApexTls::NormaliseFingerprint(Options.Fingerprint, Want);
		ApexTls::NormaliseFingerprint(Fingerprint, Have);
		if (Want != Have)
		{
			OutError = FString::Printf(
				TEXT("the server's certificate is not the one pinned in settings.yml (server.tls_fingerprint). ")
				TEXT("It presented SHA-256 %s"), *Fingerprint);
			return false;
		}
		return true;
	}

	if (!Options.bVerify)
	{
		return true;
	}

	const long Verdict = SSL_get_verify_result(Ssl);
	if (Verdict != X509_V_OK)
	{
		OutError = FString::Printf(
			TEXT("the server's certificate is not trusted (%s). If this is your own or a LAN server with a ")
			TEXT("self-signed certificate, pin it in settings.yml: server.tls_fingerprint: %s"),
			UTF8_TO_TCHAR(X509_verify_cert_error_string(Verdict)), *Fingerprint);
		return false;
	}
	return true;
}

bool FApexTlsSession::Write(const uint8* Bytes, int32 Num, FString& OutError)
{
	int32 Offset = 0;
	while (Offset < Num)
	{
		const int Written = SSL_write(Ssl, Bytes + Offset, Num - Offset);
		if (Written <= 0)
		{
			OutError = ErrorString(Written);
			return false;
		}
		Offset += Written;
	}
	return true;
}

FApexTlsSession::ERead FApexTlsSession::ReadAvailable(TArray<uint8>& Out, FString& OutError)
{
	uint8 Chunk[16 * 1024];
	for (;;)
	{
		const int Read = SSL_read(Ssl, Chunk, sizeof(Chunk));
		if (Read > 0)
		{
			Out.Append(Chunk, Read);
			continue;
		}
		const int Code = SSL_get_error(Ssl, Read);
		if (Code == SSL_ERROR_WANT_READ || Code == SSL_ERROR_WANT_WRITE)
		{
			return ERead::Ok;
		}
		if (Code == SSL_ERROR_ZERO_RETURN)
		{
			return ERead::Closed;
		}
		OutError = ErrorString(Read);
		return ERead::Failed;
	}
}

void FApexTlsSession::FeedCiphertext(const uint8* Bytes, int32 Num)
{
	if (Num > 0)
	{
		BIO_write(ReadBio, Bytes, Num);
	}
}

bool FApexTlsSession::TakeCiphertext(TArray<uint8>& Out)
{
	const int Pending = BIO_ctrl_pending(WriteBio);
	if (Pending <= 0)
	{
		return false;
	}
	const int32 Offset = Out.Num();
	Out.AddUninitialized(Pending);
	const int Read = BIO_read(WriteBio, Out.GetData() + Offset, Pending);
	Out.SetNum(Offset + FMath::Max(Read, 0), EAllowShrinking::No);
	return Read > 0;
}

void FApexTlsSession::Close()
{
	if (Ssl && SSL_is_init_finished(Ssl))
	{
		SSL_shutdown(Ssl);
	}
	ERR_clear_error();
}

FString FApexTlsSession::DescribeCipher() const
{
	if (!Ssl)
	{
		return FString();
	}
	return FString::Printf(TEXT("%s, %s"),
		UTF8_TO_TCHAR(SSL_get_version(Ssl)), UTF8_TO_TCHAR(SSL_get_cipher_name(Ssl)));
}

const TCHAR* FApexTlsSession::TrustDescription() const
{
	if (Options.IsPinned())
	{
		return TEXT("certificate matches the pinned fingerprint");
	}
	return Options.bVerify
		? TEXT("certificate verified against the system's CA roots and the host name")
		: TEXT("certificate NOT verified (server.tls_verify: false)");
}
