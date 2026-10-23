using UnrealBuildTool;

public class ApexSimNet : ModuleRules
{
	public ApexSimNet(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Sockets",
			"Networking",
			// A version 2 stream file's column blocks are unpacked by the
			// server's code (apx_stream_block_raw); without the library,
			// WITH_APEX_PHYSICS 0, version 1 files still play.
			"ApexPhysics",
		});

		// TLS on the TCP connection (ApexTlsSession.cpp): the OpenSSL the engine
		// ships, linked into this module. Not the engine's SSL module: its
		// context factory only works in monolithic builds, and the editor is not.
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
	}
}
