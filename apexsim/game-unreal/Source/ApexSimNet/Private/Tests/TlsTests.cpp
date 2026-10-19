#include "Misc/AutomationTest.h"

#include "ApexTls.h"
#include "ApexTlsSession.h"

#if WITH_DEV_AUTOMATION_TESTS

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif
THIRD_PARTY_INCLUDES_START
// OpenSSL's UI typedef clashes with Unreal's UI namespace (as in the
// engine's BuildPatchServices Crypto.cpp).
#define UI UI_ST
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/x509.h>
#undef UI
THIRD_PARTY_INCLUDES_END
#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace ApexTlsTestsPrivate
{
	constexpr EAutomationTestFlags Flags =
		EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter;

	/** 0x00..0x1F, so the formatted spelling is predictable. */
	TArray<uint8> CountingDigest()
	{
		TArray<uint8> Digest;
		for (int32 Index = 0; Index < 32; ++Index)
		{
			Digest.Add(static_cast<uint8>(Index * 7 + 3));
		}
		return Digest;
	}

	/**
	 * A TLS server on memory BIOs with a freshly made self-signed certificate
	 * (P-256, CN=localhost): what an ApexSim server with a dev certificate is
	 * to the client, without a socket.
	 */
	struct FServer
	{
		SSL_CTX* Context = nullptr;
		SSL* Ssl = nullptr;
		BIO* In = nullptr;
		BIO* Out = nullptr;
		FString Fingerprint;

		bool Init()
		{
			EVP_PKEY* Key = EVP_PKEY_new();
			EC_KEY* Ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
			if (!Key || !Ec || EC_KEY_generate_key(Ec) != 1 || EVP_PKEY_assign_EC_KEY(Key, Ec) != 1)
			{
				return false;
			}
			X509* Cert = X509_new();
			ASN1_INTEGER_set(X509_get_serialNumber(Cert), 1);
			X509_gmtime_adj(X509_getm_notBefore(Cert), -60);
			X509_gmtime_adj(X509_getm_notAfter(Cert), 3600);
			X509_set_pubkey(Cert, Key);
			X509_NAME* Name = X509_get_subject_name(Cert);
			X509_NAME_add_entry_by_txt(Name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
			X509_set_issuer_name(Cert, Name);
			X509_sign(Cert, Key, EVP_sha256());

			uint8 Digest[EVP_MAX_MD_SIZE];
			unsigned int Length = 0;
			X509_digest(Cert, EVP_sha256(), Digest, &Length);
			Fingerprint = ApexTls::FormatFingerprint(TArrayView<const uint8>(Digest, static_cast<int32>(Length)));

			Context = SSL_CTX_new(TLS_server_method());
			const bool bOk = Context
				&& SSL_CTX_use_certificate(Context, Cert) == 1
				&& SSL_CTX_use_PrivateKey(Context, Key) == 1;
			X509_free(Cert);
			EVP_PKEY_free(Key);
			if (!bOk)
			{
				return false;
			}
			Ssl = SSL_new(Context);
			In = BIO_new(BIO_s_mem());
			Out = BIO_new(BIO_s_mem());
			BIO_set_mem_eof_return(In, -1);
			SSL_set_bio(Ssl, In, Out);
			SSL_set_accept_state(Ssl);
			return true;
		}

		~FServer()
		{
			if (Ssl) { SSL_free(Ssl); }
			if (Context) { SSL_CTX_free(Context); }
			ERR_clear_error();
		}

		void Feed(const TArray<uint8>& Bytes)
		{
			if (Bytes.Num() > 0)
			{
				BIO_write(In, Bytes.GetData(), Bytes.Num());
			}
		}

		TArray<uint8> Take()
		{
			TArray<uint8> Bytes;
			const int Pending = BIO_ctrl_pending(Out);
			if (Pending > 0)
			{
				Bytes.SetNumUninitialized(Pending);
				BIO_read(Out, Bytes.GetData(), Pending);
			}
			return Bytes;
		}
	};

	/** Runs the handshake between the two to the end, or 20 flights. */
	FApexTlsSession::EStep Handshake(FApexTlsSession& Client, FServer& Server, FString& OutError)
	{
		FApexTlsSession::EStep Step = FApexTlsSession::EStep::WantRead;
		for (int32 Round = 0; Round < 20; ++Round)
		{
			Step = Client.StepHandshake(OutError);
			TArray<uint8> Up;
			Client.TakeCiphertext(Up);
			Server.Feed(Up);
			SSL_do_handshake(Server.Ssl);
			const TArray<uint8> Down = Server.Take();
			Client.FeedCiphertext(Down.GetData(), Down.Num());
			if (Step != FApexTlsSession::EStep::WantRead && SSL_is_init_finished(Server.Ssl))
			{
				break;
			}
			if (Step == FApexTlsSession::EStep::Failed)
			{
				break;
			}
		}
		return Step;
	}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexTlsModeTest,
	"ApexSim.Net.Tls.Mode",
	ApexTlsTestsPrivate::Flags)

bool FApexTlsModeTest::RunTest(const FString& Parameters)
{
	EApexTlsMode Mode = EApexTlsMode::Off;
	TestTrue(TEXT("auto parses"), ApexTls::ParseMode(TEXT("auto"), Mode));
	TestTrue(TEXT("auto is auto"), Mode == EApexTlsMode::Auto);
	TestTrue(TEXT("ON parses, any case"), ApexTls::ParseMode(TEXT(" ON "), Mode));
	TestTrue(TEXT("on is on"), Mode == EApexTlsMode::On);
	TestTrue(TEXT("false parses as off"), ApexTls::ParseMode(TEXT("false"), Mode));
	TestTrue(TEXT("false is off"), Mode == EApexTlsMode::Off);
	TestFalse(TEXT("maybe is not a mode"), ApexTls::ParseMode(TEXT("maybe"), Mode));
	TestTrue(TEXT("a refused value leaves the mode"), Mode == EApexTlsMode::Off);

	for (const EApexTlsMode Each : {EApexTlsMode::Auto, EApexTlsMode::On, EApexTlsMode::Off})
	{
		EApexTlsMode Back = EApexTlsMode::Auto;
		TestTrue(TEXT("every name parses"), ApexTls::ParseMode(ApexTls::ModeName(Each), Back));
		TestTrue(TEXT("and comes back as itself"), Back == Each);
	}
	TestTrue(TEXT("the default is auto, verified, unpinned"),
		FApexTlsOptions().Mode == EApexTlsMode::Auto && FApexTlsOptions().bVerify && !FApexTlsOptions().IsPinned());
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexTlsFingerprintTest,
	"ApexSim.Net.Tls.Fingerprint",
	ApexTlsTestsPrivate::Flags)

bool FApexTlsFingerprintTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Digest = ApexTlsTestsPrivate::CountingDigest();
	const FString Spelt = ApexTls::FormatFingerprint(Digest);
	TestEqual(TEXT("formatted as upper-case pairs with colons"), Spelt.Len(), 95);
	TestTrue(TEXT("starts 03:0A:11"), Spelt.StartsWith(TEXT("03:0A:11:")));

	FString Hex;
	TestTrue(TEXT("the formatted spelling normalises"), ApexTls::NormaliseFingerprint(Spelt, Hex));
	TestEqual(TEXT("to 64 hex digits"), Hex.Len(), 64);
	TestFalse(TEXT("with no colons left"), Hex.Contains(TEXT(":")));

	// Every spelling a person might paste.
	TestTrue(TEXT("lower case, no separators"), ApexTls::FingerprintMatches(Hex.ToLower(), Digest));
	TestTrue(TEXT("spaces between bytes"), ApexTls::FingerprintMatches(Spelt.Replace(TEXT(":"), TEXT(" ")), Digest));
	TestTrue(TEXT("openssl's own output"),
		ApexTls::FingerprintMatches(TEXT("SHA256 Fingerprint=") + Spelt, Digest));
	TestTrue(TEXT("a sha256: prefix"), ApexTls::FingerprintMatches(TEXT("sha256:") + Hex, Digest));

	TArray<uint8> Other = Digest;
	Other[31] ^= 0x01;
	TestFalse(TEXT("one bit different does not match"), ApexTls::FingerprintMatches(Spelt, Other));
	TestFalse(TEXT("a short digest never matches"),
		ApexTls::FingerprintMatches(Spelt, TArrayView<const uint8>(Digest.GetData(), 20)));

	FString Out = TEXT("untouched");
	TestFalse(TEXT("31 bytes is not a fingerprint"), ApexTls::NormaliseFingerprint(Hex.Left(62), Out));
	TestFalse(TEXT("a non-hex digit is refused"), ApexTls::NormaliseFingerprint(Hex.Left(63) + TEXT("G"), Out));
	TestFalse(TEXT("SHA-1 length is refused"), ApexTls::NormaliseFingerprint(Hex.Left(40), Out));
	TestEqual(TEXT("a refusal leaves Out alone"), Out, FString(TEXT("untouched")));

	FString Canonical;
	TestTrue(TEXT("canonical spelling from lower-case hex"), ApexTls::CanonicalFingerprint(Hex.ToLower(), Canonical));
	TestEqual(TEXT("is the formatted one"), Canonical, Spelt);
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexTlsFallbackTest,
	"ApexSim.Net.Tls.Fallback",
	ApexTlsTestsPrivate::Flags)

bool FApexTlsFallbackTest::RunTest(const FString& Parameters)
{
	// How a failed attempt is read.
	TestTrue(TEXT("nothing back is a closed hello"),
		ApexTls::ClassifyFailure(0, 0, EApexTlsFailure::Handshake) == EApexTlsFailure::ClosedBeforeReply);
	TestTrue(TEXT("a frame length back is not TLS"),
		ApexTls::ClassifyFailure(6, 0x00, EApexTlsFailure::Handshake) == EApexTlsFailure::NotTls);
	TestTrue(TEXT("an HTTP answer is not TLS"),
		ApexTls::ClassifyFailure(12, 'H', EApexTlsFailure::Handshake) == EApexTlsFailure::NotTls);
	TestTrue(TEXT("an alert record is a TLS failure"),
		ApexTls::ClassifyFailure(7, 0x15, EApexTlsFailure::Handshake) == EApexTlsFailure::Handshake);
	TestTrue(TEXT("a handshake record is TLS"), ApexTls::LooksLikeTlsRecord(0x16));
	TestFalse(TEXT("0x19 is not a record type"), ApexTls::LooksLikeTlsRecord(0x19));

	FApexTlsOptions Auto;
	FApexTlsOptions On;
	On.Mode = EApexTlsMode::On;
	FApexTlsOptions Pinned;
	Pinned.Fingerprint = ApexTls::FormatFingerprint(ApexTlsTestsPrivate::CountingDigest());
	FApexTlsOptions Unverified;
	Unverified.bVerify = false;

	TestTrue(TEXT("auto falls back from a plaintext server that drops the hello"),
		ApexTls::ShouldFallBackToPlaintext(Auto, EApexTlsFailure::ClosedBeforeReply));
	TestTrue(TEXT("auto falls back from a server answering in plaintext"),
		ApexTls::ShouldFallBackToPlaintext(Auto, EApexTlsFailure::NotTls));
	TestTrue(TEXT("tls_verify: false does not change the fallback"),
		ApexTls::ShouldFallBackToPlaintext(Unverified, EApexTlsFailure::NotTls));
	TestFalse(TEXT("auto never falls back from an untrusted certificate"),
		ApexTls::ShouldFallBackToPlaintext(Auto, EApexTlsFailure::Untrusted));
	TestFalse(TEXT("nor from a TLS handshake that failed"),
		ApexTls::ShouldFallBackToPlaintext(Auto, EApexTlsFailure::Handshake));
	TestFalse(TEXT("nor from a timeout"),
		ApexTls::ShouldFallBackToPlaintext(Auto, EApexTlsFailure::Timeout));
	TestFalse(TEXT("on never falls back"),
		ApexTls::ShouldFallBackToPlaintext(On, EApexTlsFailure::ClosedBeforeReply));
	TestFalse(TEXT("a pin makes auto behave as on"),
		ApexTls::ShouldFallBackToPlaintext(Pinned, EApexTlsFailure::ClosedBeforeReply));

	TestTrue(TEXT("an untrusted certificate stops the reconnects"), ApexTls::IsPermanent(EApexTlsFailure::Untrusted));
	TestFalse(TEXT("a closed hello does not"), ApexTls::IsPermanent(EApexTlsFailure::ClosedBeforeReply));
	TestFalse(TEXT("nor a timeout"), ApexTls::IsPermanent(EApexTlsFailure::Timeout));
	return true;
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FApexTlsSessionTest,
	"ApexSim.Net.Tls.Session",
	ApexTlsTestsPrivate::Flags)

bool FApexTlsSessionTest::RunTest(const FString& Parameters)
{
	using ApexTlsTestsPrivate::FServer;

	// The first flight is a TLS handshake record, and a plaintext answer to it
	// (an ApexSim frame) fails the handshake as not-TLS.
	{
		FApexTlsSession Client;
		FString Error;
		TestTrue(TEXT("a session sets up"), Client.Init(TEXT("127.0.0.1"), FApexTlsOptions(), Error));
		TestTrue(TEXT("the hello waits for an answer"), Client.StepHandshake(Error) == FApexTlsSession::EStep::WantRead);
		TArray<uint8> Hello;
		TestTrue(TEXT("there is a hello to send"), Client.TakeCiphertext(Hello));
		TestTrue(TEXT("it is a TLS handshake record"), Hello.Num() > 5 && Hello[0] == 0x16 && Hello[1] == 0x03);

		const uint8 Frame[] = {0x00, 0x00, 0x00, 0x04, 0x81, 0xA1, 0x41, 0x01};
		Client.FeedCiphertext(Frame, UE_ARRAY_COUNT(Frame));
		TestTrue(TEXT("a plaintext frame fails the handshake"),
			Client.StepHandshake(Error) == FApexTlsSession::EStep::Failed);
		TestTrue(TEXT("and reads as not TLS"),
			ApexTls::ClassifyFailure(UE_ARRAY_COUNT(Frame), Frame[0], EApexTlsFailure::Handshake) == EApexTlsFailure::NotTls);
	}

	// Against a real TLS server with a self-signed certificate.
	struct FCase
	{
		const TCHAR* What;
		bool bVerify;
		bool bPinRight;
		bool bPinWrong;
		bool bTrusted;
	};
	const FCase Cases[] = {
		{TEXT("pinned to its fingerprint"), true, true, false, true},
		{TEXT("pinned to another"), true, false, true, false},
		{TEXT("verified against the CA roots"), true, false, false, false},
		{TEXT("not verified"), false, false, false, true},
	};
	for (const FCase& Case : Cases)
	{
		FServer Server;
		if (!TestTrue(FString::Printf(TEXT("%s: the test server sets up"), Case.What), Server.Init()))
		{
			continue;
		}
		FApexTlsOptions Options;
		Options.Mode = EApexTlsMode::On;
		Options.bVerify = Case.bVerify;
		if (Case.bPinRight)
		{
			Options.Fingerprint = Server.Fingerprint.ToLower().Replace(TEXT(":"), TEXT(""));
		}
		if (Case.bPinWrong)
		{
			Options.Fingerprint = ApexTls::FormatFingerprint(ApexTlsTestsPrivate::CountingDigest());
		}

		FApexTlsSession Client;
		FString Error;
		TestTrue(FString::Printf(TEXT("%s: client sets up"), Case.What), Client.Init(TEXT("localhost"), Options, Error));
		const FApexTlsSession::EStep Step = ApexTlsTestsPrivate::Handshake(Client, Server, Error);
		if (!TestTrue(FString::Printf(TEXT("%s: the handshake completes (%s)"), Case.What, *Error),
				Step == FApexTlsSession::EStep::Done))
		{
			continue;
		}
		TestEqual(FString::Printf(TEXT("%s: the client saw the server's certificate"), Case.What),
			Client.PeerFingerprint(), Server.Fingerprint);

		FString Refusal;
		const bool bTrusted = Client.CheckPeer(Refusal);
		TestEqual(FString::Printf(TEXT("%s: trusted"), Case.What), bTrusted, Case.bTrusted);
		if (!bTrusted)
		{
			TestTrue(FString::Printf(TEXT("%s: the refusal names the certificate to pin"), Case.What),
				Refusal.Contains(Server.Fingerprint));
			continue;
		}

		// Frames both ways through the session.
		const uint8 Up[] = {0x00, 0x00, 0x00, 0x03, 'a', 'b', 'c'};
		TestTrue(TEXT("the client encrypts"), Client.Write(Up, UE_ARRAY_COUNT(Up), Error));
		TArray<uint8> Wire;
		Client.TakeCiphertext(Wire);
		Server.Feed(Wire);
		uint8 Got[64];
		const int Read = SSL_read(Server.Ssl, Got, sizeof(Got));
		TestEqual(TEXT("the server reads the frame whole"), Read, static_cast<int>(UE_ARRAY_COUNT(Up)));

		const uint8 Down[] = {0x00, 0x00, 0x00, 0x02, 'o', 'k'};
		SSL_write(Server.Ssl, Down, UE_ARRAY_COUNT(Down));
		const TArray<uint8> Back = Server.Take();
		Client.FeedCiphertext(Back.GetData(), Back.Num());
		TArray<uint8> Plain;
		TestTrue(TEXT("the client decrypts"), Client.ReadAvailable(Plain, Error) == FApexTlsSession::ERead::Ok);
		TestEqual(TEXT("the server's frame arrives whole"), Plain.Num(), static_cast<int32>(UE_ARRAY_COUNT(Down)));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
