// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameViewportClient.h"
#include "MyGameViewportClient.generated.h"

/**
 * 
 */
UCLASS(config=game)
class SPLITSCREEN_API UMyGameViewportClient : public UGameViewportClient
{
	GENERATED_BODY()
public:
	UPROPERTY(Config)
		FVector4 mobileWindowRect;
public:
	virtual void Init(struct FWorldContext& WorldContext, UGameInstance* OwningGameInstance, bool bCreateNewAudioDevice = true) override;
	virtual void Draw(FViewport* Viewport, FCanvas* SceneCanvas) override;
private:
	/** Current buffer visualization mode for this game viewport */
	FName MyCurrentBufferVisualizationMode;
};
