// Copyright GitHub, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FJsonObject;
class FJsonValue;
class UBlueprint;

class GITHUBCOPILOTUE_API FGitHubCopilotUEAssetService
{
public:
	bool InspectAsset(const FString& AssetPath, bool bBlueprintDefaults, FString& OutResult) const;
	bool ModifyAsset(const FString& AssetPath, bool bBlueprintDefaults, const TArray<TSharedPtr<FJsonValue>>& Operations, FString& OutResult);
	bool CreateAsset(const FString& AssetClass, const FString& AssetName, const FString& PackagePath, const TSharedPtr<FJsonObject>& Properties, bool bOpenEditor, FString& OutResult);

private:
	bool ResolveAssetTarget(
		const FString& AssetPath,
		bool bBlueprintDefaults,
		UObject*& OutRootAsset,
		UObject*& OutTargetObject,
		UBlueprint*& OutBlueprintAsset,
		FString& OutResolvedObjectPath,
		FString& OutError) const;

	bool SaveAsset(UObject* RootAsset, UBlueprint* BlueprintAsset, FString& OutError) const;
	bool ApplyOperation(UObject* RootAsset, UObject* TargetObject, UBlueprint* BlueprintAsset, const TSharedPtr<FJsonObject>& Operation, FString& OutMessage, FString& OutError);
};