using UnrealBuildTool;

public class OpenKombat : ModuleRules
{
	public OpenKombat(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "InputCore", "EnhancedInput", "Json" });

		// RenderCore and RHI: building skeletal meshes and textures from MKKE data at runtime
		PrivateDependencyModuleNames.AddRange(new string[] { "RenderCore", "RHI" });

		// MKKE's sound banks are MPEG audio; the engine's own decoder reads them (system W6)
		PrivateDependencyModuleNames.Add("dr_libs");

		// Particle effects draw as procedural quads (W2)
		PrivateDependencyModuleNames.Add("ProceduralMeshComponent");

		// MeshDescription and StaticMeshDescription: building stage meshes at runtime
		PrivateDependencyModuleNames.AddRange(new string[] { "MeshDescription", "StaticMeshDescription" });

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });
		
		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
