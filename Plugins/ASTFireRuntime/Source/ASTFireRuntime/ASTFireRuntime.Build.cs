using UnrealBuildTool;

public class ASTFireRuntime : ModuleRules
{
    public ASTFireRuntime(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine", "Niagara", "ProceduralMeshComponent"
        });
    }
}
