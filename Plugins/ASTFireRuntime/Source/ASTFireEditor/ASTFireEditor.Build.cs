using UnrealBuildTool;

public class ASTFireEditor : ModuleRules
{
    public ASTFireEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "Engine",
            "Niagara", "NiagaraEditor", "ASTFireRuntime"
        });
    }
}
