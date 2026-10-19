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
		});

		// TLS on the TCP connection (ApexTlsSession.cpp): the OpenSSL the engine
		// ships, linked into this module. Not the engine's SSL module: its
		// context factory only works in monolithic builds, and the editor is not.
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
	}
}
