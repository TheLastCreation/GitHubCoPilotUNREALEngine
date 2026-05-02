// Copyright GitHub, Inc. All Rights Reserved.

#include "Services/GitHubCopilotUEAssetService.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/World.h"
#include "InputAction.h"
#include "InputCoreTypes.h"
#include "InputMappingContext.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPtr.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

namespace
{
	static FString SanitizeAssetName(const FString& InName)
	{
		FString Name = InName.TrimStartAndEnd();
		Name.ReplaceInline(TEXT(" "), TEXT("_"));
		for (int32 Index = 0; Index < Name.Len(); ++Index)
		{
			const TCHAR Character = Name[Index];
			if (!(FChar::IsAlnum(Character) || Character == TCHAR('_')))
			{
				Name[Index] = TCHAR('_');
			}
		}
		return Name;
	}

	static FString NormalizePackagePath(FString PackagePath)
	{
		PackagePath.ReplaceInline(TEXT("\\"), TEXT("/"));
		PackagePath = PackagePath.TrimStartAndEnd();
		if (PackagePath.IsEmpty())
		{
			return FString();
		}
		if (!PackagePath.StartsWith(TEXT("/")))
		{
			PackagePath = TEXT("/Game/") + PackagePath;
		}
		if (PackagePath.EndsWith(TEXT("/")))
		{
			PackagePath.LeftChopInline(1);
		}
		return PackagePath;
	}

	static bool NormalizeObjectPath(const FString& InAssetPath, FString& OutObjectPath, FString& OutError)
	{
		FString AssetPath = InAssetPath.TrimStartAndEnd();
		AssetPath.ReplaceInline(TEXT("\\"), TEXT("/"));

		if (AssetPath.IsEmpty())
		{
			OutError = TEXT("Asset path is required");
			return false;
		}

		if (!AssetPath.StartsWith(TEXT("/")))
		{
			OutError = FString::Printf(TEXT("Invalid asset path '%s'. Use a Content Browser path like /Game/MyFolder/MyAsset or /Game/MyFolder/MyAsset.MyAsset."), *InAssetPath);
			return false;
		}

		if (AssetPath.Contains(TEXT(".")))
		{
			if (!FPackageName::IsValidObjectPath(AssetPath))
			{
				OutError = FString::Printf(TEXT("Invalid object path '%s'"), *InAssetPath);
				return false;
			}
			OutObjectPath = AssetPath;
			return true;
		}

		if (!FPackageName::IsValidLongPackageName(AssetPath))
		{
			OutError = FString::Printf(TEXT("Invalid package path '%s'"), *InAssetPath);
			return false;
		}

		const FString AssetName = FPackageName::GetLongPackageAssetName(AssetPath);
		OutObjectPath = AssetPath + TEXT(".") + AssetName;
		return true;
	}

	static bool IsPathUnderAssetRoot(const FString& ObjectPath, const FString& Root)
	{
		const FString ObjectPathLower = ObjectPath.ToLower();
		const FString RootLower = Root.ToLower();
		return ObjectPathLower.Equals(RootLower) ||
			ObjectPathLower.StartsWith(RootLower + TEXT("/")) ||
			ObjectPathLower.StartsWith(RootLower + TEXT("."));
	}

	static bool FindStaleTemplateAssetRoot(const FString& ObjectPath, FString& OutMatchedRoot)
	{
		const TArray<FString> StaleRoots = {
			TEXT("/Game/ThirdPerson"),
			TEXT("/Game/Variant_Combat"),
			TEXT("/Game/Variant_Platforming"),
			TEXT("/Game/Variant_SideScrolling"),
			TEXT("/Game/Input"),
			TEXT("/Game/Characters"),
			TEXT("/Game/VoxelWorld")
		};

		for (const FString& Root : StaleRoots)
		{
			if (IsPathUnderAssetRoot(ObjectPath, Root))
			{
				OutMatchedRoot = Root;
				return true;
			}
		}

		return false;
	}

	static FString MakeStaleTemplateAssetRootError(const FString& MatchedRoot)
	{
		return FString::Printf(
			TEXT("Asset access blocked because '%s' is a stale template asset root. Use current Content/Project asset paths instead, such as /Game/Project/Levels, /Game/Project/Input, /Game/Project/Gameplay/Variants, or /Game/Project/Systems/VoxelWorld."),
			*MatchedRoot);
	}

	static bool TryGetAssetDataForObjectPath(const FString& ObjectPath, FAssetData& OutAssetData)
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		OutAssetData = AssetRegistryModule.Get().GetAssetByObjectPath(FSoftObjectPath(ObjectPath));
		return OutAssetData.IsValid();
	}

	static bool IsBlueprintLikeAssetData(const FAssetData& AssetData)
	{
		return AssetData.AssetClassPath.ToString().Contains(TEXT("Blueprint"));
	}

	static bool IsObjectPathLoaded(const FString& ObjectPath)
	{
		return FindObject<UObject>(nullptr, *ObjectPath) != nullptr;
	}

	static bool IsWorldOrLevelObject(const UObject* Object)
	{
		return Object != nullptr && (Object->IsA<UWorld>() || Object->GetTypedOuter<UWorld>() != nullptr);
	}

	static UObject* FindPackageSaveRoot(UObject* Object)
	{
		if (Object == nullptr)
		{
			return nullptr;
		}

		UPackage* Package = Object->GetOutermost();
		for (UObject* Current = Object; Current != nullptr && Current != Package; Current = Current->GetOuter())
		{
			if (Current->HasAnyFlags(RF_Public | RF_Standalone))
			{
				return Current;
			}
		}

		return nullptr;
	}

	static bool IsSupportedProperty(const FProperty* Property)
	{
		if (Property == nullptr || Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Transient))
		{
			return false;
		}

		if (!Property->HasAnyPropertyFlags(CPF_Edit))
		{
			return false;
		}

		return Property->IsA<FBoolProperty>() ||
			Property->IsA<FNumericProperty>() ||
			Property->IsA<FEnumProperty>() ||
			Property->IsA<FByteProperty>() ||
			Property->IsA<FStrProperty>() ||
			Property->IsA<FNameProperty>() ||
			Property->IsA<FTextProperty>() ||
			Property->IsA<FObjectPropertyBase>() ||
			Property->IsA<FSoftObjectProperty>() ||
			Property->IsA<FArrayProperty>() ||
			Property->IsA<FStructProperty>();
	}

	static FProperty* FindEditablePropertyByName(UClass* Class, const FString& PropertyName)
	{
		if (Class == nullptr || PropertyName.IsEmpty())
		{
			return nullptr;
		}

		for (TFieldIterator<FProperty> It(Class, EFieldIteratorFlags::IncludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			if (Property != nullptr && Property->GetName().Equals(PropertyName, ESearchCase::IgnoreCase) && IsSupportedProperty(Property))
			{
				return Property;
			}
		}

		return nullptr;
	}

	static FString DescribePropertyType(const FProperty* Property)
	{
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			return FString::Printf(TEXT("array<%s>"), *DescribePropertyType(ArrayProperty->Inner));
		}

		if (const FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			return FString::Printf(TEXT("soft_object<%s>"), *SoftObjectProperty->PropertyClass->GetName());
		}

		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			return FString::Printf(TEXT("object<%s>"), *ObjectProperty->PropertyClass->GetName());
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			return FString::Printf(TEXT("struct<%s>"), *StructProperty->Struct->GetName());
		}

		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			return FString::Printf(TEXT("enum<%s>"), *EnumProperty->GetEnum()->GetName());
		}

		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			if (ByteProperty->Enum != nullptr)
			{
				return FString::Printf(TEXT("enum<%s>"), *ByteProperty->Enum->GetName());
			}
		}

		if (Property->IsA<FBoolProperty>()) return TEXT("bool");
		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			return NumericProperty->IsFloatingPoint() ? TEXT("number") : TEXT("integer");
		}
		if (Property->IsA<FStrProperty>()) return TEXT("string");
		if (Property->IsA<FNameProperty>()) return TEXT("name");
		if (Property->IsA<FTextProperty>()) return TEXT("text");

		return Property->GetClass()->GetName();
	}

	static TSharedPtr<FJsonValue> MakeJsonNullOrString(const FString& Value)
	{
		if (Value.IsEmpty())
		{
			return MakeShared<FJsonValueNull>();
		}
		return MakeShared<FJsonValueString>(Value);
	}

	static TSharedPtr<FJsonValue> ExportPropertyValueToJson(const FProperty* Property, const void* ValuePtr)
	{
		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(ValuePtr));
		}

		if (const FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			if (NumericProperty->IsInteger())
			{
				return MakeShared<FJsonValueNumber>(static_cast<double>(NumericProperty->GetSignedIntPropertyValue(ValuePtr)));
			}
			return MakeShared<FJsonValueNumber>(NumericProperty->GetFloatingPointPropertyValue(ValuePtr));
		}

		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const int64 RawValue = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			const UEnum* Enum = EnumProperty->GetEnum();
			return MakeJsonNullOrString(Enum != nullptr ? Enum->GetNameStringByValue(RawValue) : FString::FromInt(static_cast<int32>(RawValue)));
		}

		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			const uint8 RawValue = ByteProperty->GetPropertyValue(ValuePtr);
			if (ByteProperty->Enum != nullptr)
			{
				return MakeJsonNullOrString(ByteProperty->Enum->GetNameStringByValue(RawValue));
			}
			return MakeShared<FJsonValueNumber>(static_cast<double>(RawValue));
		}

		if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			return MakeShared<FJsonValueString>(StrProperty->GetPropertyValue(ValuePtr));
		}

		if (const FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			return MakeShared<FJsonValueString>(NameProperty->GetPropertyValue(ValuePtr).ToString());
		}

		if (const FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			return MakeShared<FJsonValueString>(TextProperty->GetPropertyValue(ValuePtr).ToString());
		}

		if (const FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			const FSoftObjectPtr SoftObjectPtr = SoftObjectProperty->GetPropertyValue(ValuePtr);
			return MakeJsonNullOrString(SoftObjectPtr.IsNull() ? FString() : SoftObjectPtr.ToSoftObjectPath().ToString());
		}

		if (const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			const UObject* ObjectValue = ObjectProperty->GetObjectPropertyValue(ValuePtr);
			return MakeJsonNullOrString(ObjectValue != nullptr ? ObjectValue->GetPathName() : FString());
		}

		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == TBaseStructure<FVector>::Get())
			{
				const FVector& VectorValue = *reinterpret_cast<const FVector*>(ValuePtr);
				TArray<TSharedPtr<FJsonValue>> Values;
				Values.Add(MakeShared<FJsonValueNumber>(VectorValue.X));
				Values.Add(MakeShared<FJsonValueNumber>(VectorValue.Y));
				Values.Add(MakeShared<FJsonValueNumber>(VectorValue.Z));
				return MakeShared<FJsonValueArray>(Values);
			}

			if (StructProperty->Struct == TBaseStructure<FRotator>::Get())
			{
				const FRotator& RotatorValue = *reinterpret_cast<const FRotator*>(ValuePtr);
				TArray<TSharedPtr<FJsonValue>> Values;
				Values.Add(MakeShared<FJsonValueNumber>(RotatorValue.Pitch));
				Values.Add(MakeShared<FJsonValueNumber>(RotatorValue.Yaw));
				Values.Add(MakeShared<FJsonValueNumber>(RotatorValue.Roll));
				return MakeShared<FJsonValueArray>(Values);
			}

			if (StructProperty->Struct == TBaseStructure<FLinearColor>::Get())
			{
				const FLinearColor& ColorValue = *reinterpret_cast<const FLinearColor*>(ValuePtr);
				TArray<TSharedPtr<FJsonValue>> Values;
				Values.Add(MakeShared<FJsonValueNumber>(ColorValue.R));
				Values.Add(MakeShared<FJsonValueNumber>(ColorValue.G));
				Values.Add(MakeShared<FJsonValueNumber>(ColorValue.B));
				Values.Add(MakeShared<FJsonValueNumber>(ColorValue.A));
				return MakeShared<FJsonValueArray>(Values);
			}

			FString ExportedText;
			StructProperty->ExportTextItem_Direct(ExportedText, ValuePtr, nullptr, nullptr, PPF_None);
			return MakeShared<FJsonValueString>(ExportedText);
		}

		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper ArrayHelper(ArrayProperty, ValuePtr);
			TArray<TSharedPtr<FJsonValue>> ArrayValues;
			for (int32 Index = 0; Index < ArrayHelper.Num(); ++Index)
			{
				ArrayValues.Add(ExportPropertyValueToJson(ArrayProperty->Inner, ArrayHelper.GetRawPtr(Index)));
			}
			return MakeShared<FJsonValueArray>(ArrayValues);
		}

		FString ExportedText;
		Property->ExportTextItem_Direct(ExportedText, ValuePtr, nullptr, nullptr, PPF_None);
		return MakeShared<FJsonValueString>(ExportedText);
	}

	static bool TryReadVectorFromJson(const TSharedPtr<FJsonValue>& JsonValue, FVector& OutVector)
	{
		const TArray<TSharedPtr<FJsonValue>>* ArrayValues = nullptr;
		if (!JsonValue.IsValid() || !JsonValue->TryGetArray(ArrayValues) || ArrayValues == nullptr || ArrayValues->Num() < 3)
		{
			return false;
		}

		OutVector.X = (*ArrayValues)[0]->AsNumber();
		OutVector.Y = (*ArrayValues)[1]->AsNumber();
		OutVector.Z = (*ArrayValues)[2]->AsNumber();
		return true;
	}

	static bool TryReadRotatorFromJson(const TSharedPtr<FJsonValue>& JsonValue, FRotator& OutRotator)
	{
		const TArray<TSharedPtr<FJsonValue>>* ArrayValues = nullptr;
		if (!JsonValue.IsValid() || !JsonValue->TryGetArray(ArrayValues) || ArrayValues == nullptr || ArrayValues->Num() < 3)
		{
			return false;
		}

		OutRotator.Pitch = (*ArrayValues)[0]->AsNumber();
		OutRotator.Yaw = (*ArrayValues)[1]->AsNumber();
		OutRotator.Roll = (*ArrayValues)[2]->AsNumber();
		return true;
	}

	static bool TryReadLinearColorFromJson(const TSharedPtr<FJsonValue>& JsonValue, FLinearColor& OutColor)
	{
		const TArray<TSharedPtr<FJsonValue>>* ArrayValues = nullptr;
		if (!JsonValue.IsValid() || !JsonValue->TryGetArray(ArrayValues) || ArrayValues == nullptr || ArrayValues->Num() < 3)
		{
			return false;
		}

		OutColor.R = (*ArrayValues)[0]->AsNumber();
		OutColor.G = (*ArrayValues)[1]->AsNumber();
		OutColor.B = (*ArrayValues)[2]->AsNumber();
		OutColor.A = ArrayValues->Num() >= 4 ? (*ArrayValues)[3]->AsNumber() : 1.0f;
		return true;
	}

	static UObject* ResolveObjectReference(const FString& InObjectPath, UClass* ExpectedClass, FString& OutError)
	{
		FString ObjectPath;
		if (!NormalizeObjectPath(InObjectPath, ObjectPath, OutError))
		{
			return nullptr;
		}

		FString MatchedStaleRoot;
		if (FindStaleTemplateAssetRoot(ObjectPath, MatchedStaleRoot))
		{
			OutError = MakeStaleTemplateAssetRootError(MatchedStaleRoot);
			return nullptr;
		}

		if (UObject* ExistingObject = FindObject<UObject>(nullptr, *ObjectPath))
		{
			if (ExpectedClass != nullptr && !ExistingObject->IsA(ExpectedClass))
			{
				OutError = FString::Printf(TEXT("Referenced asset '%s' is %s, expected %s"), *InObjectPath, *ExistingObject->GetClass()->GetName(), *ExpectedClass->GetName());
				return nullptr;
			}

			return ExistingObject;
		}

		const FString AssetPath = FSoftObjectPath(ObjectPath).GetAssetPathString();
		if (AssetPath.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Referenced asset '%s' is not a top-level asset path. Open the owning asset in the editor before using nested object references."), *InObjectPath);
			return nullptr;
		}

		FAssetData AssetData;
		if (!TryGetAssetDataForObjectPath(AssetPath, AssetData))
		{
			OutError = FString::Printf(TEXT("Referenced asset '%s' is not loaded and was not found in the Asset Registry. Use a top-level Content Browser asset path or open the asset in the editor first."), *InObjectPath);
			return nullptr;
		}

		if (AssetData.AssetClassPath.ToString() == TEXT("/Script/Engine.World"))
		{
			OutError = FString::Printf(TEXT("Referenced asset '%s' is a world/map asset. modify_asset does not support loading world assets through object-property assignment."), *InObjectPath);
			return nullptr;
		}

		UObject* LoadedObject = LoadObject<UObject>(nullptr, *AssetPath);
		if (LoadedObject == nullptr)
		{
			OutError = FString::Printf(TEXT("Failed to load referenced asset '%s'"), *InObjectPath);
			return nullptr;
		}

		if (ExpectedClass != nullptr && !LoadedObject->IsA(ExpectedClass))
		{
			OutError = FString::Printf(TEXT("Referenced asset '%s' is %s, expected %s"), *InObjectPath, *LoadedObject->GetClass()->GetName(), *ExpectedClass->GetName());
			return nullptr;
		}

		return LoadedObject;
	}

	static UClass* ResolveClassReference(const FString& InClassPath, UClass* ExpectedBaseClass, FString& OutError)
	{
		FString ObjectPath;
		if (!NormalizeObjectPath(InClassPath, ObjectPath, OutError))
		{
			return nullptr;
		}

		FString MatchedStaleRoot;
		if (FindStaleTemplateAssetRoot(ObjectPath, MatchedStaleRoot))
		{
			OutError = MakeStaleTemplateAssetRootError(MatchedStaleRoot);
			return nullptr;
		}

		if (UClass* ExistingClass = FindObject<UClass>(nullptr, *ObjectPath))
		{
			if (ExpectedBaseClass != nullptr && !ExistingClass->IsChildOf(ExpectedBaseClass))
			{
				OutError = FString::Printf(TEXT("Referenced class '%s' is %s, expected subclass of %s"), *InClassPath, *ExistingClass->GetName(), *ExpectedBaseClass->GetName());
				return nullptr;
			}

			return ExistingClass;
		}

		FString ClassObjectPath = ObjectPath;
		FAssetData AssetData;
		if (TryGetAssetDataForObjectPath(ObjectPath, AssetData) && IsBlueprintLikeAssetData(AssetData))
		{
			const FString PackageName = AssetData.PackageName.ToString();
			const FString AssetName = AssetData.AssetName.ToString();
			if (PackageName.IsEmpty() || AssetName.IsEmpty())
			{
				OutError = FString::Printf(TEXT("Referenced class '%s' resolved to invalid Blueprint asset metadata"), *InClassPath);
				return nullptr;
			}

			ClassObjectPath = PackageName + TEXT(".") + AssetName + TEXT("_C");
		}

		UClass* LoadedClass = LoadObject<UClass>(nullptr, *ClassObjectPath);
		if (LoadedClass == nullptr)
		{
			OutError = FString::Printf(TEXT("Failed to load referenced class '%s'"), *InClassPath);
			return nullptr;
		}

		if (ExpectedBaseClass != nullptr && !LoadedClass->IsChildOf(ExpectedBaseClass))
		{
			OutError = FString::Printf(TEXT("Referenced class '%s' is %s, expected subclass of %s"), *InClassPath, *LoadedClass->GetName(), *ExpectedBaseClass->GetName());
			return nullptr;
		}

		return LoadedClass;
	}

	static bool SetPropertyValueFromJson(FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& JsonValue, FString& OutError)
	{
		if (Property == nullptr || ValuePtr == nullptr || !JsonValue.IsValid())
		{
			OutError = TEXT("Property value is missing");
			return false;
		}

		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			BoolProperty->SetPropertyValue(ValuePtr, JsonValue->AsBool());
			return true;
		}

		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			if (NumericProperty->IsInteger())
			{
				NumericProperty->SetIntPropertyValue(ValuePtr, static_cast<int64>(JsonValue->AsNumber()));
			}
			else
			{
				NumericProperty->SetFloatingPointPropertyValue(ValuePtr, JsonValue->AsNumber());
			}
			return true;
		}

		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const UEnum* Enum = EnumProperty->GetEnum();
			int64 EnumValue = INDEX_NONE;
			if (JsonValue->Type == EJson::String && Enum != nullptr)
			{
				EnumValue = Enum->GetValueByNameString(JsonValue->AsString());
			}
			else if (JsonValue->Type == EJson::Number)
			{
				EnumValue = static_cast<int64>(JsonValue->AsNumber());
			}

			if (EnumValue == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Invalid enum value for property '%s'"), *Property->GetName());
				return false;
			}

			EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, EnumValue);
			return true;
		}

		if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			if (ByteProperty->Enum != nullptr && JsonValue->Type == EJson::String)
			{
				const int64 EnumValue = ByteProperty->Enum->GetValueByNameString(JsonValue->AsString());
				if (EnumValue == INDEX_NONE)
				{
					OutError = FString::Printf(TEXT("Invalid enum value for property '%s'"), *Property->GetName());
					return false;
				}
				ByteProperty->SetPropertyValue(ValuePtr, static_cast<uint8>(EnumValue));
				return true;
			}

			ByteProperty->SetPropertyValue(ValuePtr, static_cast<uint8>(JsonValue->AsNumber()));
			return true;
		}

		if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			StrProperty->SetPropertyValue(ValuePtr, JsonValue->AsString());
			return true;
		}

		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			NameProperty->SetPropertyValue(ValuePtr, FName(*JsonValue->AsString()));
			return true;
		}

		if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			TextProperty->SetPropertyValue(ValuePtr, FText::FromString(JsonValue->AsString()));
			return true;
		}

		if (FSoftObjectProperty* SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
		{
			if (JsonValue->IsNull())
			{
				SoftObjectProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr());
				return true;
			}

			FString ObjectPath;
			if (!NormalizeObjectPath(JsonValue->AsString(), ObjectPath, OutError))
			{
				return false;
			}

			SoftObjectProperty->SetPropertyValue(ValuePtr, FSoftObjectPtr(FSoftObjectPath(ObjectPath)));
			return true;
		}

		if (FClassProperty* ClassProperty = CastField<FClassProperty>(Property))
		{
			if (JsonValue->IsNull())
			{
				ClassProperty->SetObjectPropertyValue(ValuePtr, nullptr);
				return true;
			}

			UClass* ClassValue = ResolveClassReference(JsonValue->AsString(), ClassProperty->MetaClass, OutError);
			if (ClassValue == nullptr)
			{
				return false;
			}

			ClassProperty->SetObjectPropertyValue(ValuePtr, ClassValue);
			return true;
		}

		if (FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(Property))
		{
			if (JsonValue->IsNull())
			{
				ObjectProperty->SetObjectPropertyValue(ValuePtr, nullptr);
				return true;
			}

			UObject* ObjectValue = ResolveObjectReference(JsonValue->AsString(), ObjectProperty->PropertyClass, OutError);
			if (ObjectValue == nullptr)
			{
				return false;
			}

			ObjectProperty->SetObjectPropertyValue(ValuePtr, ObjectValue);
			return true;
		}

		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			if (StructProperty->Struct == TBaseStructure<FVector>::Get())
			{
				FVector VectorValue;
				if (!TryReadVectorFromJson(JsonValue, VectorValue))
				{
					OutError = FString::Printf(TEXT("Property '%s' expects a [X, Y, Z] array"), *Property->GetName());
					return false;
				}
				*reinterpret_cast<FVector*>(ValuePtr) = VectorValue;
				return true;
			}

			if (StructProperty->Struct == TBaseStructure<FRotator>::Get())
			{
				FRotator RotatorValue;
				if (!TryReadRotatorFromJson(JsonValue, RotatorValue))
				{
					OutError = FString::Printf(TEXT("Property '%s' expects a [Pitch, Yaw, Roll] array"), *Property->GetName());
					return false;
				}
				*reinterpret_cast<FRotator*>(ValuePtr) = RotatorValue;
				return true;
			}

			if (StructProperty->Struct == TBaseStructure<FLinearColor>::Get())
			{
				FLinearColor ColorValue;
				if (!TryReadLinearColorFromJson(JsonValue, ColorValue))
				{
					OutError = FString::Printf(TEXT("Property '%s' expects a [R, G, B, A?] array"), *Property->GetName());
					return false;
				}
				*reinterpret_cast<FLinearColor*>(ValuePtr) = ColorValue;
				return true;
			}

			if (JsonValue->Type == EJson::String)
			{
				const FString TextValue = JsonValue->AsString();
				if (StructProperty->ImportText_Direct(*TextValue, ValuePtr, nullptr, PPF_None) != nullptr)
				{
					return true;
				}
			}

			OutError = FString::Printf(TEXT("Struct property '%s' is only supported for FVector, FRotator, FLinearColor, or importable text values"), *Property->GetName());
			return false;
		}

		OutError = FString::Printf(TEXT("Property type '%s' is not currently supported"), *Property->GetClass()->GetName());
		return false;
	}

	static bool SetArrayElementFromJson(FArrayProperty* ArrayProperty, void* ElementPtr, const TSharedPtr<FJsonValue>& JsonValue, FString& OutError)
	{
		return SetPropertyValueFromJson(ArrayProperty->Inner, ElementPtr, JsonValue, OutError);
	}

	static bool TryApplyEnhancedInputOperation(UObject* TargetObject, const TSharedPtr<FJsonObject>& Operation, FString& OutMessage, FString& OutError)
	{
		UInputMappingContext* MappingContext = Cast<UInputMappingContext>(TargetObject);
		if (MappingContext == nullptr)
		{
			return false;
		}

		FString OperationType;
		if (!Operation->TryGetStringField(TEXT("type"), OperationType) || !OperationType.Equals(TEXT("add_mapping"), ESearchCase::IgnoreCase))
		{
			return false;
		}

		FString ActionPath;
		if (!Operation->TryGetStringField(TEXT("action"), ActionPath) || ActionPath.IsEmpty())
		{
			OutError = TEXT("Enhanced Input add_mapping requires an 'action' asset path");
			return true;
		}

		FString KeyName;
		if (!Operation->TryGetStringField(TEXT("key"), KeyName) || KeyName.IsEmpty())
		{
			OutError = TEXT("Enhanced Input add_mapping requires a 'key' field");
			return true;
		}

		FString ResolveError;
		UObject* LoadedActionObject = ResolveObjectReference(ActionPath, UInputAction::StaticClass(), ResolveError);
		if (LoadedActionObject == nullptr)
		{
			OutError = ResolveError;
			return true;
		}

		UInputAction* InputAction = Cast<UInputAction>(LoadedActionObject);
		const FKey MappingKey(*KeyName);
		if (!MappingKey.IsValid())
		{
			OutError = FString::Printf(TEXT("Invalid key '%s'"), *KeyName);
			return true;
		}

		MappingContext->Modify();
		MappingContext->MapKey(InputAction, MappingKey);
		OutMessage = FString::Printf(TEXT("Added mapping %s -> %s"), *InputAction->GetPathName(), *MappingKey.GetDisplayName().ToString());
		return true;
	}

	static FString SerializeJsonObject(const TSharedPtr<FJsonObject>& JsonObject)
	{
		FString Output;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(JsonObject.ToSharedRef(), Writer);
		return Output;
	}

	static FString BuildAssetRegistryInspectionResult(const FAssetData& AssetData, const FString& ResolvedObjectPath, bool bBlueprintDefaults, const FString& SkipReason)
	{
		TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
		ResultObject->SetStringField(TEXT("resolved_object_path"), ResolvedObjectPath);
		ResultObject->SetStringField(TEXT("target_kind"), bBlueprintDefaults ? TEXT("blueprint_defaults_registry") : TEXT("asset_registry"));
		ResultObject->SetStringField(TEXT("package_name"), AssetData.PackageName.ToString());
		ResultObject->SetStringField(TEXT("package_path"), AssetData.PackagePath.ToString());
		ResultObject->SetStringField(TEXT("asset_name"), AssetData.AssetName.ToString());
		ResultObject->SetStringField(TEXT("asset_class_path"), AssetData.AssetClassPath.ToString());
		ResultObject->SetBoolField(TEXT("loaded"), false);
		ResultObject->SetBoolField(TEXT("load_skipped"), true);
		ResultObject->SetStringField(TEXT("skip_reason"), SkipReason);

		TSharedPtr<FJsonObject> TagsObject = MakeShared<FJsonObject>();
		AssetData.TagsAndValues.ForEach([TagsObject](TPair<FName, FAssetTagValueRef> Pair)
		{
			TagsObject->SetStringField(Pair.Key.ToString(), Pair.Value.GetValue());
		});
		ResultObject->SetObjectField(TEXT("tags"), TagsObject);

		TArray<TSharedPtr<FJsonValue>> PropertyArray;
		ResultObject->SetArrayField(TEXT("properties"), PropertyArray);
		return SerializeJsonObject(ResultObject);
	}
}

bool FGitHubCopilotUEAssetService::ResolveAssetTarget(
	const FString& AssetPath,
	bool bBlueprintDefaults,
	UObject*& OutRootAsset,
	UObject*& OutTargetObject,
	UBlueprint*& OutBlueprintAsset,
	FString& OutResolvedObjectPath,
	FString& OutError) const
{
	OutRootAsset = nullptr;
	OutTargetObject = nullptr;
	OutBlueprintAsset = nullptr;
	OutResolvedObjectPath.Reset();

	if (!NormalizeObjectPath(AssetPath, OutResolvedObjectPath, OutError))
	{
		return false;
	}

	FString MatchedStaleRoot;
	if (FindStaleTemplateAssetRoot(OutResolvedObjectPath, MatchedStaleRoot))
	{
		OutError = MakeStaleTemplateAssetRootError(MatchedStaleRoot);
		return false;
	}

	FAssetData AssetData;
	const bool bHasAssetData = TryGetAssetDataForObjectPath(OutResolvedObjectPath, AssetData);
	OutRootAsset = FindObject<UObject>(nullptr, *OutResolvedObjectPath);
	if (OutRootAsset == nullptr)
	{
		if (bHasAssetData && IsBlueprintLikeAssetData(AssetData))
		{
			OutError = FString::Printf(
				TEXT("Refusing to load unloaded Blueprint asset '%s' through this tool. inspect_asset can return Asset Registry metadata without loading it; open or repair the Blueprint in the editor before editing defaults."),
				*OutResolvedObjectPath);
			return false;
		}

		OutRootAsset = LoadObject<UObject>(nullptr, *OutResolvedObjectPath);
	}
	if (OutRootAsset == nullptr)
	{
		OutError = FString::Printf(TEXT("Asset not found: %s"), *OutResolvedObjectPath);
		return false;
	}

	if (!bBlueprintDefaults)
	{
		OutTargetObject = OutRootAsset;
		return true;
	}

	OutBlueprintAsset = Cast<UBlueprint>(OutRootAsset);
	if (OutBlueprintAsset == nullptr)
	{
		OutError = FString::Printf(TEXT("Asset '%s' is %s. blueprint_defaults=true requires a Blueprint asset."), *OutResolvedObjectPath, *OutRootAsset->GetClass()->GetName());
		return false;
	}

	if (OutBlueprintAsset->GeneratedClass == nullptr)
	{
		OutError = FString::Printf(TEXT("Blueprint '%s' does not have a generated class loaded. Compile or repair the Blueprint in the editor before editing defaults."), *OutResolvedObjectPath);
		return false;
	}

	OutTargetObject = OutBlueprintAsset->GeneratedClass->GetDefaultObject();
	if (OutTargetObject == nullptr)
	{
		OutError = FString::Printf(TEXT("Failed to resolve Blueprint default object for '%s'"), *OutResolvedObjectPath);
		return false;
	}

	return true;
}

bool FGitHubCopilotUEAssetService::SaveAsset(UObject* RootAsset, UBlueprint* BlueprintAsset, FString& OutError) const
{
	OutError.Reset();

	if (RootAsset == nullptr)
	{
		OutError = TEXT("Cannot save a null asset");
		return false;
	}

	if (BlueprintAsset != nullptr)
	{
		FKismetEditorUtilities::CompileBlueprint(BlueprintAsset);
	}

	RootAsset->MarkPackageDirty();
	UPackage* Package = RootAsset->GetOutermost();
	if (Package == nullptr)
	{
		OutError = TEXT("Asset package is null");
		return false;
	}

	Package->MarkPackageDirty();

	if (IsWorldOrLevelObject(RootAsset))
	{
		OutError = TEXT("Level/world package was marked dirty but not auto-saved by modify_asset. Save the level from the editor; this avoids Unreal's RF_Standalone data-loss guard for map subobjects such as WorldSettings.");
		return true;
	}

	UObject* SaveRoot = BlueprintAsset != nullptr ? BlueprintAsset : FindPackageSaveRoot(RootAsset);
	if (SaveRoot == nullptr)
	{
		OutError = FString::Printf(
			TEXT("Object '%s' was marked dirty but not auto-saved because it is not a top-level asset with RF_Public or RF_Standalone. Use the top-level asset path when saving through modify_asset."),
			*RootAsset->GetPathName());
		return true;
	}

	SaveRoot->MarkPackageDirty();
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_None;
	SaveArgs.bSlowTask = false;

	if (!UPackage::SavePackage(Package, SaveRoot, *PackageFilename, SaveArgs))
	{
		OutError = FString::Printf(TEXT("Failed to save package '%s'"), *PackageFilename);
		return false;
	}

	return true;
}

bool FGitHubCopilotUEAssetService::InspectAsset(const FString& AssetPath, bool bBlueprintDefaults, FString& OutResult) const
{
	FString PreflightObjectPath;
	FString PreflightError;
	if (!NormalizeObjectPath(AssetPath, PreflightObjectPath, PreflightError))
	{
		OutResult = PreflightError;
		return false;
	}

	FString MatchedStaleRoot;
	if (FindStaleTemplateAssetRoot(PreflightObjectPath, MatchedStaleRoot))
	{
		OutResult = MakeStaleTemplateAssetRootError(MatchedStaleRoot);
		return false;
	}

	FAssetData AssetData;
	const bool bHasAssetData = TryGetAssetDataForObjectPath(PreflightObjectPath, AssetData);
	const bool bIsLoaded = IsObjectPathLoaded(PreflightObjectPath);
	if (!bHasAssetData && !bIsLoaded)
	{
		OutResult = FString::Printf(
			TEXT("Asset '%s' is not loaded and was not found in the Asset Registry. inspect_asset only supports loaded objects or top-level asset paths recognized by the Content Browser; open the asset in the editor first or inspect the top-level asset path instead."),
			*PreflightObjectPath);
		return false;
	}

	if (bHasAssetData && !bIsLoaded)
	{
		if (bBlueprintDefaults && !IsBlueprintLikeAssetData(AssetData))
		{
			OutResult = FString::Printf(
				TEXT("Asset '%s' is %s. blueprint_defaults=true requires a Blueprint asset."),
				*PreflightObjectPath,
				*AssetData.AssetClassPath.ToString());
			return false;
		}

		OutResult = BuildAssetRegistryInspectionResult(
			AssetData,
			PreflightObjectPath,
			bBlueprintDefaults,
			TEXT("Asset is not loaded; returning Asset Registry metadata to avoid loading packages during inspect_asset."));
		return true;
	}

	UObject* RootAsset = nullptr;
	UObject* TargetObject = nullptr;
	UBlueprint* BlueprintAsset = nullptr;
	FString ResolvedObjectPath;
	FString Error;
	if (!ResolveAssetTarget(AssetPath, bBlueprintDefaults, RootAsset, TargetObject, BlueprintAsset, ResolvedObjectPath, Error))
	{
		OutResult = Error;
		return false;
	}

	TSharedPtr<FJsonObject> ResultObject = MakeShared<FJsonObject>();
	ResultObject->SetStringField(TEXT("resolved_object_path"), ResolvedObjectPath);
	ResultObject->SetStringField(TEXT("target_kind"), bBlueprintDefaults ? TEXT("blueprint_defaults") : TEXT("asset"));
	ResultObject->SetStringField(TEXT("root_asset_class"), RootAsset->GetClass()->GetName());
	ResultObject->SetStringField(TEXT("target_class"), TargetObject->GetClass()->GetName());

	TArray<TSharedPtr<FJsonValue>> PropertyArray;
	for (TFieldIterator<FProperty> It(TargetObject->GetClass(), EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		FProperty* Property = *It;
		if (!IsSupportedProperty(Property))
		{
			continue;
		}

		TSharedPtr<FJsonObject> PropertyObject = MakeShared<FJsonObject>();
		PropertyObject->SetStringField(TEXT("name"), Property->GetName());
		PropertyObject->SetStringField(TEXT("type"), DescribePropertyType(Property));
		PropertyObject->SetBoolField(TEXT("editable"), true);
		PropertyObject->SetField(TEXT("value"), ExportPropertyValueToJson(Property, Property->ContainerPtrToValuePtr<void>(TargetObject)));
		PropertyArray.Add(MakeShared<FJsonValueObject>(PropertyObject));
	}

	ResultObject->SetArrayField(TEXT("properties"), PropertyArray);
	OutResult = SerializeJsonObject(ResultObject);
	return true;
}

bool FGitHubCopilotUEAssetService::ApplyOperation(UObject* RootAsset, UObject* TargetObject, UBlueprint* BlueprintAsset, const TSharedPtr<FJsonObject>& Operation, FString& OutMessage, FString& OutError)
{
	if (!Operation.IsValid())
	{
		OutError = TEXT("Operation entry is not an object");
		return false;
	}

	if (TryApplyEnhancedInputOperation(TargetObject, Operation, OutMessage, OutError))
	{
		return OutError.IsEmpty();
	}

	FString OperationType;
	if (!Operation->TryGetStringField(TEXT("type"), OperationType) || OperationType.IsEmpty())
	{
		OutError = TEXT("Each operation requires a 'type' field");
		return false;
	}

	FString PropertyName;
	if (!Operation->TryGetStringField(TEXT("property"), PropertyName) || PropertyName.IsEmpty())
	{
		OutError = FString::Printf(TEXT("Operation '%s' requires a 'property' field"), *OperationType);
		return false;
	}

	FProperty* Property = FindEditablePropertyByName(TargetObject->GetClass(), PropertyName);
	if (Property == nullptr)
	{
		OutError = FString::Printf(TEXT("Editable property '%s' was not found on %s"), *PropertyName, *TargetObject->GetClass()->GetName());
		return false;
	}

	TargetObject->Modify();
	if (RootAsset != nullptr && RootAsset != TargetObject)
	{
		RootAsset->Modify();
	}
	if (BlueprintAsset != nullptr)
	{
		BlueprintAsset->Modify();
	}

	if (OperationType.Equals(TEXT("clear_property"), ESearchCase::IgnoreCase))
	{
		Property->ClearValue_InContainer(TargetObject);
		OutMessage = FString::Printf(TEXT("Cleared property %s"), *PropertyName);
		return true;
	}

	if (FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
	{
		FScriptArrayHelper ArrayHelper(ArrayProperty, Property->ContainerPtrToValuePtr<void>(TargetObject));

		if (OperationType.Equals(TEXT("add_array_item"), ESearchCase::IgnoreCase))
		{
			const TSharedPtr<FJsonValue>* ValuePtr = Operation->Values.Find(TEXT("value"));
			if (ValuePtr == nullptr || !ValuePtr->IsValid())
			{
				OutError = FString::Printf(TEXT("Operation '%s' requires a 'value' field"), *OperationType);
				return false;
			}

			const int32 NewIndex = ArrayHelper.AddValue();
			if (!SetArrayElementFromJson(ArrayProperty, ArrayHelper.GetRawPtr(NewIndex), *ValuePtr, OutError))
			{
				ArrayHelper.RemoveValues(NewIndex, 1);
				return false;
			}

			OutMessage = FString::Printf(TEXT("Added item to array property %s"), *PropertyName);
			return true;
		}

		if (OperationType.Equals(TEXT("remove_array_item"), ESearchCase::IgnoreCase))
		{
			int32 Index = INDEX_NONE;
			if (!Operation->TryGetNumberField(TEXT("index"), Index) || !ArrayHelper.IsValidIndex(Index))
			{
				OutError = FString::Printf(TEXT("Operation '%s' requires a valid 'index' for property %s"), *OperationType, *PropertyName);
				return false;
			}

			ArrayHelper.RemoveValues(Index, 1);
			OutMessage = FString::Printf(TEXT("Removed index %d from array property %s"), Index, *PropertyName);
			return true;
		}

		if (OperationType.Equals(TEXT("replace_array_item"), ESearchCase::IgnoreCase))
		{
			int32 Index = INDEX_NONE;
			if (!Operation->TryGetNumberField(TEXT("index"), Index) || !ArrayHelper.IsValidIndex(Index))
			{
				OutError = FString::Printf(TEXT("Operation '%s' requires a valid 'index' for property %s"), *OperationType, *PropertyName);
				return false;
			}

			const TSharedPtr<FJsonValue>* ValuePtr = Operation->Values.Find(TEXT("value"));
			if (ValuePtr == nullptr || !ValuePtr->IsValid())
			{
				OutError = FString::Printf(TEXT("Operation '%s' requires a 'value' field"), *OperationType);
				return false;
			}

			if (!SetArrayElementFromJson(ArrayProperty, ArrayHelper.GetRawPtr(Index), *ValuePtr, OutError))
			{
				return false;
			}

			OutMessage = FString::Printf(TEXT("Replaced index %d in array property %s"), Index, *PropertyName);
			return true;
		}
	}

	if (!OperationType.Equals(TEXT("set_property"), ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(TEXT("Unsupported operation type '%s'"), *OperationType);
		return false;
	}

	const TSharedPtr<FJsonValue>* ValuePtr = Operation->Values.Find(TEXT("value"));
	if (ValuePtr == nullptr || !ValuePtr->IsValid())
	{
		OutError = TEXT("set_property requires a 'value' field");
		return false;
	}

	if (!SetPropertyValueFromJson(Property, Property->ContainerPtrToValuePtr<void>(TargetObject), *ValuePtr, OutError))
	{
		return false;
	}

	OutMessage = FString::Printf(TEXT("Set property %s"), *PropertyName);
	return true;
}

bool FGitHubCopilotUEAssetService::ModifyAsset(const FString& AssetPath, bool bBlueprintDefaults, const TArray<TSharedPtr<FJsonValue>>& Operations, FString& OutResult)
{
	if (Operations.Num() == 0)
	{
		OutResult = TEXT("At least one operation is required");
		return false;
	}

	UObject* RootAsset = nullptr;
	UObject* TargetObject = nullptr;
	UBlueprint* BlueprintAsset = nullptr;
	FString ResolvedObjectPath;
	FString Error;
	if (!ResolveAssetTarget(AssetPath, bBlueprintDefaults, RootAsset, TargetObject, BlueprintAsset, ResolvedObjectPath, Error))
	{
		OutResult = Error;
		return false;
	}

	TargetObject->PreEditChange(nullptr);
	if (RootAsset != nullptr && RootAsset != TargetObject)
	{
		RootAsset->PreEditChange(nullptr);
	}

	TArray<FString> AppliedMessages;
	for (const TSharedPtr<FJsonValue>& OperationValue : Operations)
	{
		const TSharedPtr<FJsonObject>* OperationObject = nullptr;
		if (!OperationValue.IsValid() || !OperationValue->TryGetObject(OperationObject) || !OperationObject || !OperationObject->IsValid())
		{
			OutResult = TEXT("Each operations entry must be an object");
			return false;
		}

		FString AppliedMessage;
		if (!ApplyOperation(RootAsset, TargetObject, BlueprintAsset, *OperationObject, AppliedMessage, Error))
		{
			OutResult = Error;
			return false;
		}

		AppliedMessages.Add(AppliedMessage);
	}

	TargetObject->PostEditChange();
	if (RootAsset != nullptr && RootAsset != TargetObject)
	{
		RootAsset->PostEditChange();
	}

	if (!SaveAsset(RootAsset, BlueprintAsset, Error))
	{
		OutResult = Error;
		return false;
	}
	if (!Error.IsEmpty())
	{
		AppliedMessages.Add(Error);
	}

	FString Message = FString::Printf(TEXT("Modified %s (%s)"), *ResolvedObjectPath, bBlueprintDefaults ? TEXT("blueprint defaults") : TEXT("asset instance"));
	for (const FString& Applied : AppliedMessages)
	{
		Message += TEXT("\n- ") + Applied;
	}

	OutResult = Message;
	return true;
}

bool FGitHubCopilotUEAssetService::CreateAsset(const FString& AssetClass, const FString& AssetName, const FString& PackagePath, const TSharedPtr<FJsonObject>& Properties, bool bOpenEditor, FString& OutResult)
{
	const FString SanitizedAssetName = SanitizeAssetName(AssetName);
	if (SanitizedAssetName.IsEmpty())
	{
		OutResult = TEXT("asset_name must contain at least one valid character");
		return false;
	}

	const FString NormalizedPackagePath = NormalizePackagePath(PackagePath.IsEmpty() ? TEXT("/Game/Copilot") : PackagePath);
	if (NormalizedPackagePath.IsEmpty() || !FPackageName::IsValidLongPackageName(NormalizedPackagePath))
	{
		OutResult = FString::Printf(TEXT("Invalid package_path '%s'. Use /Game/..."), *PackagePath);
		return false;
	}

	FString NormalizedClass = AssetClass.TrimStartAndEnd().ToLower();
	UClass* NewAssetClass = nullptr;
	if (NormalizedClass == TEXT("inputaction") || NormalizedClass == TEXT("uinputaction") || NormalizedClass == TEXT("/script/enhancedinput.inputaction"))
	{
		NewAssetClass = UInputAction::StaticClass();
	}
	else if (NormalizedClass == TEXT("inputmappingcontext") || NormalizedClass == TEXT("uinputmappingcontext") || NormalizedClass == TEXT("/script/enhancedinput.inputmappingcontext"))
	{
		NewAssetClass = UInputMappingContext::StaticClass();
	}

	if (NewAssetClass == nullptr)
	{
		OutResult = FString::Printf(TEXT("Unsupported asset_class '%s'. Supported values: InputAction, InputMappingContext"), *AssetClass);
		return false;
	}

	const FString PackageName = NormalizedPackagePath / SanitizedAssetName;
	const FString ObjectPath = PackageName + TEXT(".") + SanitizedAssetName;
	if (LoadObject<UObject>(nullptr, *ObjectPath) != nullptr)
	{
		OutResult = FString::Printf(TEXT("Asset already exists: %s"), *ObjectPath);
		return false;
	}

	UPackage* Package = CreatePackage(*PackageName);
	if (Package == nullptr)
	{
		OutResult = FString::Printf(TEXT("Failed to create package '%s'"), *PackageName);
		return false;
	}

	UObject* NewAsset = NewObject<UObject>(Package, NewAssetClass, FName(*SanitizedAssetName), RF_Public | RF_Standalone);
	if (NewAsset == nullptr)
	{
		OutResult = FString::Printf(TEXT("Failed to create asset '%s'"), *SanitizedAssetName);
		return false;
	}

	if (Properties.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Properties->Values)
		{
			FProperty* Property = FindEditablePropertyByName(NewAsset->GetClass(), Pair.Key);
			if (Property == nullptr)
			{
				continue;
			}

			FString Error;
			if (!SetPropertyValueFromJson(Property, Property->ContainerPtrToValuePtr<void>(NewAsset), Pair.Value, Error))
			{
				OutResult = Error;
				return false;
			}
		}
	}

	NewAsset->PostEditChange();
	FAssetRegistryModule::AssetCreated(NewAsset);
	Package->MarkPackageDirty();

	FString SaveError;
	if (!SaveAsset(NewAsset, nullptr, SaveError))
	{
		OutResult = SaveError;
		return false;
	}

	if (bOpenEditor && GEditor)
	{
		TArray<UObject*> AssetsToOpen;
		AssetsToOpen.Add(NewAsset);
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAssets(AssetsToOpen);
	}

	OutResult = FString::Printf(TEXT("Created asset '%s'\nObject path: %s\nClass: %s"), *SanitizedAssetName, *ObjectPath, *NewAsset->GetClass()->GetName());
	return true;
}