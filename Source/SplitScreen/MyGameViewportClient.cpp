// Fill out your copyright notice in the Description page of Project Settings.


#include "MyGameViewportClient.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CoreDelegates.h"
#include "Misc/App.h"
#include "GameMapsSettings.h"
#include "EngineStats.h"
#include "RenderingThread.h"
#include "SceneView.h"
#include "LegacyScreenPercentageDriver.h"
#include "AI/NavigationSystemBase.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "GameFramework/Volume.h"
#include "Components/SkeletalMeshComponent.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "SceneManagement.h"
#include "Particles/ParticleSystemComponent.h"
#include "Engine/NetDriver.h"
#include "Engine/LocalPlayer.h"
#include "ContentStreaming.h"
#include "UnrealEngine.h"
#include "EngineUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/SViewport.h"
#include "Engine/Console.h"
#include "GameFramework/HUD.h"
#include "FXSystem.h"
#include "SubtitleManager.h"
#include "ImageUtils.h"
#include "SceneViewExtension.h"
#include "IHeadMountedDisplay.h"
#include "IXRTrackingSystem.h"
#include "EngineModule.h"
#include "AudioDeviceManager.h"
#include "AudioDevice.h"
#include "Audio/AudioDebug.h"
#include "Sound/SoundWave.h"
#include "HighResScreenshot.h"
#include "BufferVisualizationData.h"
#include "GameFramework/InputSettings.h"
#include "Components/LineBatchComponent.h"
#include "Debug/DebugDrawService.h"
#include "Components/BrushComponent.h"
#include "Engine/GameEngine.h"
#include "Logging/MessageLog.h"
#include "GameFramework/GameUserSettings.h"
#include "Engine/UserInterfaceSettings.h"
#include "Slate/SceneViewport.h"
#include "Slate/SGameLayerManager.h"
#include "ActorEditorUtils.h"
#include "ComponentRecreateRenderStateContext.h"
#include "DynamicResolutionState.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "HAL/PlatformApplicationMisc.h"
#include "CustomStaticScreenPercentage.h"

#if WITH_EDITOR
#include "Settings/LevelEditorPlaySettings.h"
#endif
#include "Math/UnrealMathUtility.h"

DECLARE_CYCLE_STAT(TEXT("UI Drawing Time"), STAT_MyUIDrawingTime, STATGROUP_UI);

void UMyGameViewportClient::Init(FWorldContext& WorldContext, UGameInstance* OwningGameInstance, bool bCreateNewAudioDevice)
{
	SplitscreenInfo[ESplitScreenType::None].PlayerData.Empty();
	SplitscreenInfo[ESplitScreenType::None].PlayerData.Add(FPerPlayerSplitscreenData(mobileWindowRect.X, mobileWindowRect.Y, 
		mobileWindowRect.Z, mobileWindowRect.W));
	Super::Init(WorldContext, OwningGameInstance, bCreateNewAudioDevice);
}

static UCanvas* GetCanvasByName(FName CanvasName)
{
	// Cache to avoid FString/FName conversions/compares
	static TMap<FName, UCanvas*> CanvasMap;
	UCanvas** FoundCanvas = CanvasMap.Find(CanvasName);
	if (!FoundCanvas)
	{
		UCanvas* CanvasObject = FindObject<UCanvas>(GetTransientPackage(), *CanvasName.ToString());
		if (!CanvasObject)
		{
			CanvasObject = NewObject<UCanvas>(GetTransientPackage(), CanvasName);
			CanvasObject->AddToRoot();
		}

		CanvasMap.Add(CanvasName, CanvasObject);
		return CanvasObject;
	}

	return *FoundCanvas;
}
void UMyGameViewportClient::Draw(FViewport* InViewport, FCanvas* SceneCanvas)
{
	check(SceneCanvas);

	FSimpleMulticastDelegate& DrawDelegateBegin = OnBeginDraw();
	DrawDelegateBegin.Broadcast();

	const bool bStereoRendering = GEngine->IsStereoscopic3D(InViewport);
	FCanvas* DebugCanvas = InViewport->GetDebugCanvas();

	// Create a temporary canvas if there isn't already one.
	static FName CanvasObjectName(TEXT("CanvasObject"));
	UCanvas* CanvasObject = GetCanvasByName(CanvasObjectName);
	CanvasObject->Canvas = SceneCanvas;

	// Create temp debug canvas object
	FIntPoint DebugCanvasSize = InViewport->GetSizeXY();
	if (bStereoRendering && GEngine->XRSystem.IsValid() && GEngine->XRSystem->GetHMDDevice())
	{
		DebugCanvasSize = GEngine->XRSystem->GetHMDDevice()->GetIdealDebugCanvasRenderTargetSize();
	}

	static FName DebugCanvasObjectName(TEXT("DebugCanvasObject"));
	UCanvas* DebugCanvasObject = GetCanvasByName(DebugCanvasObjectName);
	DebugCanvasObject->Init(DebugCanvasSize.X, DebugCanvasSize.Y, NULL, DebugCanvas);

	if (DebugCanvas)
	{
		DebugCanvas->SetScaledToRenderTarget(bStereoRendering);
		DebugCanvas->SetStereoRendering(bStereoRendering);
	}
	if (SceneCanvas)
	{
		SceneCanvas->SetScaledToRenderTarget(bStereoRendering);
		SceneCanvas->SetStereoRendering(bStereoRendering);
	}

	UWorld* MyWorld = GetWorld();
	if (MyWorld == nullptr)
	{
		return;
	}

	// Force path tracing view mode, and extern code set path tracer show flags
	const bool bForcePathTracing = InViewport->GetClient()->GetEngineShowFlags()->PathTracing;
	if (bForcePathTracing)
	{
		EngineShowFlags.SetPathTracing(true);
		ViewModeIndex = VMI_PathTracing;
	}

	// create the view family for rendering the world scene to the viewport's render target
	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		InViewport,
		MyWorld->Scene,
		EngineShowFlags)
		.SetRealtimeUpdate(true));

#if WITH_EDITOR
	if (GIsEditor)
	{
		// Force enable view family show flag for HighDPI derived's screen percentage.
		ViewFamily.EngineShowFlags.ScreenPercentage = true;
	}
#endif

	ViewFamily.ViewExtensions = GEngine->ViewExtensions->GatherActiveExtensions(FSceneViewExtensionContext(InViewport));

	for (auto ViewExt : ViewFamily.ViewExtensions)
	{
		ViewExt->SetupViewFamily(ViewFamily);
	}

	if (bStereoRendering && GEngine->XRSystem.IsValid() && GEngine->XRSystem->GetHMDDevice())
	{
		// Allow HMD to modify screen settings
		GEngine->XRSystem->GetHMDDevice()->UpdateScreenSettings(Viewport);
	}

	ESplitScreenType::Type SplitScreenConfig = GetCurrentSplitscreenConfiguration();
	ViewFamily.ViewMode = EViewModeIndex(ViewModeIndex);
	EngineShowFlagOverride(ESFIM_Game, ViewFamily.ViewMode, ViewFamily.EngineShowFlags, false);

	if (ViewFamily.EngineShowFlags.VisualizeBuffer && AllowDebugViewmodes())
	{
		// Process the buffer visualization console command
		FName NewBufferVisualizationMode = NAME_None;
		static IConsoleVariable* ICVar = IConsoleManager::Get().FindConsoleVariable(FBufferVisualizationData::GetVisualizationTargetConsoleCommandName());
		if (ICVar)
		{
			static const FName OverviewName = TEXT("Overview");
			FString ModeNameString = ICVar->GetString();
			FName ModeName = *ModeNameString;
			if (ModeNameString.IsEmpty() || ModeName == OverviewName || ModeName == NAME_None)
			{
				NewBufferVisualizationMode = NAME_None;
			}
			else
			{
				if (GetBufferVisualizationData().GetMaterial(ModeName) == NULL)
				{
					// Mode is out of range, so display a message to the user, and reset the mode back to the previous valid one
					UE_LOG(LogConsoleResponse, Warning, TEXT("Buffer visualization mode '%s' does not exist"), *ModeNameString);
					NewBufferVisualizationMode = MyCurrentBufferVisualizationMode;
					// todo: cvars are user settings, here the cvar state is used to avoid log spam and to auto correct for the user (likely not what the user wants)
					ICVar->Set(*NewBufferVisualizationMode.GetPlainNameString(), ECVF_SetByCode);
				}
				else
				{
					NewBufferVisualizationMode = ModeName;
				}
			}
		}

		if (NewBufferVisualizationMode != MyCurrentBufferVisualizationMode)
		{
			MyCurrentBufferVisualizationMode = NewBufferVisualizationMode;
		}
	}

	// Setup the screen percentage and upscaling method for the view family.
	bool bFinalScreenPercentageShowFlag;
	bool bUsesDynamicResolution = false;
	{
		checkf(ViewFamily.GetScreenPercentageInterface() == nullptr,
			TEXT("Some code has tried to set up an alien screen percentage driver, that could be wrong if not supported very well by the RHI."));

		// Force screen percentage show flag to be turned off if not supported.
		if (!ViewFamily.SupportsScreenPercentage())
		{
			ViewFamily.EngineShowFlags.ScreenPercentage = false;
		}

		// Set up secondary resolution fraction for the view family.
		if (!bStereoRendering && ViewFamily.SupportsScreenPercentage())
		{
			float CustomSecondaruScreenPercentage = 0;

			if (CustomSecondaruScreenPercentage > 0.0)
			{
				// Override secondary resolution fraction with CVar.
				ViewFamily.SecondaryViewFraction = FMath::Min(CustomSecondaruScreenPercentage / 100.0f, 1.0f);
			}
			else
			{
				// Automatically compute secondary resolution fraction from DPI.
				ViewFamily.SecondaryViewFraction = GetDPIDerivedResolutionFraction();
			}

			check(ViewFamily.SecondaryViewFraction > 0.0f);
		}

		// Setup main view family with screen percentage interface by dynamic resolution if screen percentage is enabled.
#if WITH_DYNAMIC_RESOLUTION
		if (ViewFamily.EngineShowFlags.ScreenPercentage)
		{
			FDynamicResolutionStateInfos DynamicResolutionStateInfos;
			GEngine->GetDynamicResolutionCurrentStateInfos(/* out */ DynamicResolutionStateInfos);

			// Do not allow dynamic resolution to touch the view family if not supported to ensure there is no possibility to ruin
			// game play experience on platforms that does not support it, but have it enabled by mistake.
			if (DynamicResolutionStateInfos.Status == EDynamicResolutionStatus::Enabled)
			{
				GEngine->EmitDynamicResolutionEvent(EDynamicResolutionStateEvent::BeginDynamicResolutionRendering);
				GEngine->GetDynamicResolutionState()->SetupMainViewFamily(ViewFamily);

				bUsesDynamicResolution = ViewFamily.GetScreenPercentageInterface() != nullptr;
			}
			else if (DynamicResolutionStateInfos.Status == EDynamicResolutionStatus::DebugForceEnabled)
			{
				GEngine->EmitDynamicResolutionEvent(EDynamicResolutionStateEvent::BeginDynamicResolutionRendering);
				ViewFamily.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(
					ViewFamily,
					DynamicResolutionStateInfos.ResolutionFractionApproximation,
					/* AllowPostProcessSettingsScreenPercentage = */ false,
					DynamicResolutionStateInfos.ResolutionFractionUpperBound));

				bUsesDynamicResolution = true;
			}

#if CSV_PROFILER
			if (DynamicResolutionStateInfos.ResolutionFractionApproximation >= 0.0f)
			{
				CSV_CUSTOM_STAT_GLOBAL(DynamicResolutionPercentage, DynamicResolutionStateInfos.ResolutionFractionApproximation * 100.0f, ECsvCustomStatOp::Set);
			}
#endif
		}
#endif

		if (GCustomStaticScreenPercentage && ViewFamily.ViewMode == EViewModeIndex::VMI_Lit)
		{
			GCustomStaticScreenPercentage->SetupMainGameViewFamily(ViewFamily);
		}

		// If a screen percentage interface was not set by dynamic resolution, then create one matching legacy behavior.
		if (ViewFamily.GetScreenPercentageInterface() == nullptr)
		{
			bool AllowPostProcessSettingsScreenPercentage = false;
			float GlobalResolutionFraction = 1.0f;

			if (ViewFamily.EngineShowFlags.ScreenPercentage)
			{
				// Allow FPostProcessSettings::ScreenPercentage.
				AllowPostProcessSettingsScreenPercentage = true;

				// Get global view fraction set by r.ScreenPercentage.
				GlobalResolutionFraction = FLegacyScreenPercentageDriver::GetCVarResolutionFraction();
			}

			ViewFamily.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(
				ViewFamily, GlobalResolutionFraction, AllowPostProcessSettingsScreenPercentage));
		}

		check(ViewFamily.GetScreenPercentageInterface() != nullptr);

		bFinalScreenPercentageShowFlag = ViewFamily.EngineShowFlags.ScreenPercentage;
	}

	TMap<ULocalPlayer*, FSceneView*> PlayerViewMap;
	TArray<FSceneView*> Views;

	for (FLocalPlayerIterator Iterator(GEngine, MyWorld); Iterator; ++Iterator)
	{
		ULocalPlayer* LocalPlayer = *Iterator;
		if (LocalPlayer)
		{
			APlayerController* PlayerController = LocalPlayer->PlayerController;

			const bool bEnableStereo = GEngine->IsStereoscopic3D(InViewport);
			const int32 NumViews = bStereoRendering ? GEngine->StereoRenderingDevice->GetDesiredNumberOfViews(bStereoRendering) : 1;

			for (int32 i = 0; i < NumViews; ++i)
			{
				// Calculate the player's view information.
				FVector		ViewLocation;
				FRotator	ViewRotation;

				EStereoscopicPass PassType = bStereoRendering ? GEngine->StereoRenderingDevice->GetViewPassForIndex(bStereoRendering, i) : eSSP_FULL;

				FSceneView* View = LocalPlayer->CalcSceneView(&ViewFamily, ViewLocation, ViewRotation, InViewport, nullptr, PassType);

				if (View)
				{
					Views.Add(View);

					if (View->Family->EngineShowFlags.Wireframe)
					{
						// Wireframe color is emissive-only, and mesh-modifying materials do not use material substitution, hence...
						View->DiffuseOverrideParameter = FVector4(0.f, 0.f, 0.f, 0.f);
						View->SpecularOverrideParameter = FVector4(0.f, 0.f, 0.f, 0.f);
					}
					else if (View->Family->EngineShowFlags.OverrideDiffuseAndSpecular)
					{
						View->DiffuseOverrideParameter = FVector4(GEngine->LightingOnlyBrightness.R, GEngine->LightingOnlyBrightness.G, GEngine->LightingOnlyBrightness.B, 0.0f);
						View->SpecularOverrideParameter = FVector4(.1f, .1f, .1f, 0.0f);
					}
					else if (View->Family->EngineShowFlags.LightingOnlyOverride)
					{
						View->DiffuseOverrideParameter = FVector4(GEngine->LightingOnlyBrightness.R, GEngine->LightingOnlyBrightness.G, GEngine->LightingOnlyBrightness.B, 0.0f);
						View->SpecularOverrideParameter = FVector4(0.f, 0.f, 0.f, 0.f);
					}
					else if (View->Family->EngineShowFlags.ReflectionOverride)
					{
						View->DiffuseOverrideParameter = FVector4(0.f, 0.f, 0.f, 0.f);
						View->SpecularOverrideParameter = FVector4(1, 1, 1, 0.0f);
						View->NormalOverrideParameter = FVector4(0, 0, 1, 0.0f);
						View->RoughnessOverrideParameter = FVector2D(0.0f, 0.0f);
					}

					if (!View->Family->EngineShowFlags.Diffuse)
					{
						View->DiffuseOverrideParameter = FVector4(0.f, 0.f, 0.f, 0.f);
					}

					if (!View->Family->EngineShowFlags.Specular)
					{
						View->SpecularOverrideParameter = FVector4(0.f, 0.f, 0.f, 0.f);
					}

					View->CurrentBufferVisualizationMode = MyCurrentBufferVisualizationMode;

					View->CameraConstrainedViewRect = View->UnscaledViewRect;

					// If this is the primary drawing pass, update things that depend on the view location
					if (i == 0)
					{
						// Save the location of the view.
						LocalPlayer->LastViewLocation = ViewLocation;

						PlayerViewMap.Add(LocalPlayer, View);

						// Update the listener.
						if (AudioDevice && PlayerController != NULL)
						{
							bool bUpdateListenerPosition = true;

							// If the main audio device is used for multiple PIE viewport clients, we only
							// want to update the main audio device listener position if it is in focus
							if (GEngine)
							{
								FAudioDeviceManager* AudioDeviceManager = GEngine->GetAudioDeviceManager();

								// If there is more than one world referencing the main audio device
								if (AudioDeviceManager->GetNumMainAudioDeviceWorlds() > 1)
								{
									Audio::FDeviceId MainAudioDeviceID = GEngine->GetMainAudioDeviceID();
									if (AudioDevice->DeviceID == MainAudioDeviceID && !bHasAudioFocus)
									{
										bUpdateListenerPosition = false;
									}
								}
							}

							if (bUpdateListenerPosition)
							{
								FVector Location;
								FVector ProjFront;
								FVector ProjRight;
								PlayerController->GetAudioListenerPosition(/*out*/ Location, /*out*/ ProjFront, /*out*/ ProjRight);

								FTransform ListenerTransform(FRotationMatrix::MakeFromXY(ProjFront, ProjRight));

								// Allow the HMD to adjust based on the head position of the player, as opposed to the view location
								if (GEngine->XRSystem.IsValid() && GEngine->StereoRenderingDevice.IsValid() && GEngine->StereoRenderingDevice->IsStereoEnabled())
								{
									const FVector Offset = GEngine->XRSystem->GetAudioListenerOffset();
									Location += ListenerTransform.TransformPositionNoScale(Offset);
								}

								ListenerTransform.SetTranslation(Location);
								ListenerTransform.NormalizeRotation();

								uint32 ViewportIndex = PlayerViewMap.Num() - 1;
								AudioDevice->SetListener(MyWorld, ViewportIndex, ListenerTransform, (View->bCameraCut ? 0.f : MyWorld->GetDeltaSeconds()));

								FVector OverrideAttenuation;
								if (PlayerController->GetAudioListenerAttenuationOverridePosition(OverrideAttenuation))
								{
									AudioDevice->SetListenerAttenuationOverride(ViewportIndex, OverrideAttenuation);
								}
								else
								{
									AudioDevice->ClearListenerAttenuationOverride(ViewportIndex);
								}
							}
						}

#if RHI_RAYTRACING
						View->SetupRayTracedRendering();
#endif
					}

					// Add view information for resource streaming. Allow up to 5X boost for small FOV.
					const float StreamingScale = 1.f / FMath::Clamp<float>(View->LODDistanceFactor, .2f, 1.f);
					IStreamingManager::Get().AddViewInformation(View->ViewMatrices.GetViewOrigin(), View->UnscaledViewRect.Width(), View->UnscaledViewRect.Width() * View->ViewMatrices.GetProjectionMatrix().M[0][0], StreamingScale);
					MyWorld->ViewLocationsRenderedLastFrame.Add(View->ViewMatrices.GetViewOrigin());
				}
			}
		}
	}

#if CSV_PROFILER
	UpdateCsvCameraStats(PlayerViewMap);
#endif

	FinalizeViews(&ViewFamily, PlayerViewMap);

	// Update level streaming.
	MyWorld->UpdateLevelStreaming();

	// Find largest rectangle bounded by all rendered views.
	uint32 MinX = InViewport->GetSizeXY().X, MinY = InViewport->GetSizeXY().Y, MaxX = 0, MaxY = 0;
	uint32 TotalArea = 0;
	{
		for (int32 ViewIndex = 0; ViewIndex < ViewFamily.Views.Num(); ++ViewIndex)
		{
			const FSceneView* View = ViewFamily.Views[ViewIndex];

			FIntRect UpscaledViewRect = View->UnscaledViewRect;

			MinX = FMath::Min<uint32>(UpscaledViewRect.Min.X, MinX);
			MinY = FMath::Min<uint32>(UpscaledViewRect.Min.Y, MinY);
			MaxX = FMath::Max<uint32>(UpscaledViewRect.Max.X, MaxX);
			MaxY = FMath::Max<uint32>(UpscaledViewRect.Max.Y, MaxY);
			TotalArea += FMath::TruncToInt(UpscaledViewRect.Width()) * FMath::TruncToInt(UpscaledViewRect.Height());
		}

		// To draw black borders around the rendered image (prevents artifacts from post processing passes that read outside of the image e.g. PostProcessAA)
		{
			int32 BlackBorders = 0;

			if (ViewFamily.Views.Num() == 1 && BlackBorders)
			{
				MinX += BlackBorders;
				MinY += BlackBorders;
				MaxX -= BlackBorders;
				MaxY -= BlackBorders;
				TotalArea = (MaxX - MinX) * (MaxY - MinY);
			}
		}
	}

	// If the views don't cover the entire bounding rectangle, clear the entire buffer.
	bool bBufferCleared = true;
	bool bStereoscopicPass = (ViewFamily.Views.Num() != 0 && IStereoRendering::IsStereoEyeView(*ViewFamily.Views[0]));
	if (ViewFamily.Views.Num() == 0 || TotalArea != (MaxX - MinX) * (MaxY - MinY) || bDisableWorldRendering || bStereoscopicPass)
	{
		if (bDisableWorldRendering || !bStereoscopicPass) // TotalArea computation does not work correctly for stereoscopic views
		{
			SceneCanvas->Clear(FLinearColor::Transparent);
		}

		bBufferCleared = true;
	}

	{
		// Make sure the engine show flag for screen percentage is still what it was when setting up the screen percentage interface
		ViewFamily.EngineShowFlags.ScreenPercentage = bFinalScreenPercentageShowFlag;

		if (bStereoRendering && bUsesDynamicResolution)
		{
			// Change screen percentage method to raw output when doing dynamic resolution with VR if not using TAA upsample.
			for (FSceneView* View : Views)
			{
				if (View->PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::SpatialUpscale)
				{
					View->PrimaryScreenPercentageMethod = EPrimaryScreenPercentageMethod::RawOutput;
				}
			}
		}
	}

	ViewFamily.bIsHDR = GetWindow().IsValid() ? GetWindow().Get()->GetIsHDR() : false;

	// Draw the player views.
	if (!bDisableWorldRendering && PlayerViewMap.Num() > 0 && FSlateApplication::Get().GetPlatformApplication()->IsAllowedToRender()) //-V560
	{
		GetRendererModule().BeginRenderingViewFamily(SceneCanvas, &ViewFamily);
	}
	else
	{
		GetRendererModule().PerFrameCleanupIfSkipRenderer();

		// Make sure RHI resources get flushed if we're not using a renderer
		ENQUEUE_RENDER_COMMAND(UGameViewportClient_FlushRHIResources)(
			[](FRHICommandListImmediate& RHICmdList)
			{
				RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
			});
	}

	// Beyond this point, only UI rendering independent from dynamc resolution.
	GEngine->EmitDynamicResolutionEvent(EDynamicResolutionStateEvent::EndDynamicResolutionRendering);

	// Clear areas of the rendertarget (backbuffer) that aren't drawn over by the views.
	if (!bBufferCleared)
	{
		// clear left
		if (MinX > 0)
		{
#if PLATFORM_ANDROID
			SceneCanvas->DrawTile(0, 0, MinX, InViewport->GetSizeXY().Y, 0.0f, 0.0f, 1.0f, 1.f, FLinearColor::Black, NULL, false);
#else
			SceneCanvas->DrawTile(0, 0, MinX, InViewport->GetSizeXY().Y, 0.0f, 0.0f, 1.0f, 1.f, FLinearColor::Black, NULL, false);
#endif
		}
		// clear right
		if (MaxX < (uint32)InViewport->GetSizeXY().X)
		{
#if PLATFORM_ANDROID
			SceneCanvas->DrawTile(MaxX, InViewport->GetSizeXY().Y, InViewport->GetSizeXY().X, 0, 0.0f, 0.0f, 1.0f, 1.f, FLinearColor::Black, NULL, false);
#else
			SceneCanvas->DrawTile(MaxX, 0, InViewport->GetSizeXY().X, InViewport->GetSizeXY().Y, 0.0f, 0.0f, 1.0f, 1.f, FLinearColor::Black, NULL, false);
#endif
			}
		// clear top
		if (MinY > 0)
		{
#if PLATFORM_ANDROID
			SceneCanvas->DrawTile(MinX, InViewport->GetSizeXY().Y, MaxX, InViewport->GetSizeXY().Y - MinY, 0.0f, 0.0f, 1.0f, 1.f, FLinearColor::Black, NULL, false);
#else
			SceneCanvas->DrawTile(MinX, 0, MaxX, MinY, 0.0f, 0.0f, 1.0f, 1.f, FLinearColor::Black, NULL, false);
#endif
		}
		// clear bottom
		if (MaxY < (uint32)InViewport->GetSizeXY().Y)
		{
#if PLATFORM_ANDROID
			SceneCanvas->DrawTile(MinX, InViewport->GetSizeXY().Y - MaxY, MaxX, 0, 0.0f, 0.0f, 1.0f, 1.f, FLinearColor::Black, NULL, false);
#else
			SceneCanvas->DrawTile(MinX, MaxY, MaxX, InViewport->GetSizeXY().Y, 0.0f, 0.0f, 1.0f, 1.f, FLinearColor::Black, NULL, false);
#endif
		}
	}

	// Remove temporary debug lines.
	if (MyWorld->LineBatcher != nullptr)
	{
		MyWorld->LineBatcher->Flush();
	}

	if (MyWorld->ForegroundLineBatcher != nullptr)
	{
		MyWorld->ForegroundLineBatcher->Flush();
	}

	// Draw FX debug information.
	if (MyWorld->FXSystem)
	{
		MyWorld->FXSystem->DrawDebug(SceneCanvas);
	}

	// Render the UI.
	if (FSlateApplication::Get().GetPlatformApplication()->IsAllowedToRender())
	{
		SCOPE_CYCLE_COUNTER(STAT_MyUIDrawingTime);
		CSV_SCOPED_TIMING_STAT_EXCLUSIVE(UI);

		// render HUD
		bool bDisplayedSubtitles = false;
		for (FConstPlayerControllerIterator Iterator = MyWorld->GetPlayerControllerIterator(); Iterator; ++Iterator)
		{
			APlayerController* PlayerController = Iterator->Get();
			if (PlayerController)
			{
				ULocalPlayer* LocalPlayer = Cast<ULocalPlayer>(PlayerController->Player);
				if (LocalPlayer)
				{
					FSceneView* View = PlayerViewMap.FindRef(LocalPlayer);
					if (View != NULL)
					{
						// rendering to directly to viewport target
						FVector CanvasOrigin(FMath::TruncToFloat(View->UnscaledViewRect.Min.X), FMath::TruncToInt(View->UnscaledViewRect.Min.Y), 0.f);

						CanvasObject->Init(View->UnscaledViewRect.Width(), View->UnscaledViewRect.Height(), View, SceneCanvas);

						// Set the canvas transform for the player's view rectangle.
						check(SceneCanvas);
						SceneCanvas->PushAbsoluteTransform(FTranslationMatrix(CanvasOrigin));
						CanvasObject->ApplySafeZoneTransform();

						// Render the player's HUD.
						if (PlayerController->MyHUD)
						{
							//SCOPE_CYCLE_COUNTER(STAT_HudTime);

							DebugCanvasObject->SceneView = View;
							PlayerController->MyHUD->SetCanvas(CanvasObject, DebugCanvasObject);

							PlayerController->MyHUD->PostRender();

							// Put these pointers back as if a blueprint breakpoint hits during HUD PostRender they can
							// have been changed
							CanvasObject->Canvas = SceneCanvas;
							DebugCanvasObject->Canvas = DebugCanvas;

							// A side effect of PostRender is that the playercontroller could be destroyed
							if (!PlayerController->IsPendingKill())
							{
								PlayerController->MyHUD->SetCanvas(NULL, NULL);
							}
						}

						if (DebugCanvas != NULL)
						{
							DebugCanvas->PushAbsoluteTransform(FTranslationMatrix(CanvasOrigin));
							UDebugDrawService::Draw(ViewFamily.EngineShowFlags, InViewport, View, DebugCanvas, DebugCanvasObject);
							DebugCanvas->PopTransform();
						}

						CanvasObject->PopSafeZoneTransform();
						SceneCanvas->PopTransform();

						// draw subtitles
						if (!bDisplayedSubtitles)
						{
							FVector2D MinPos(0.f, 0.f);
							FVector2D MaxPos(1.f, 1.f);
							GetSubtitleRegion(MinPos, MaxPos);

							const uint32 SizeX = SceneCanvas->GetRenderTarget()->GetSizeXY().X;
							const uint32 SizeY = SceneCanvas->GetRenderTarget()->GetSizeXY().Y;
							FIntRect SubtitleRegion(FMath::TruncToInt(SizeX * MinPos.X), FMath::TruncToInt(SizeY * MinPos.Y), FMath::TruncToInt(SizeX * MaxPos.X), FMath::TruncToInt(SizeY * MaxPos.Y));
							FSubtitleManager::GetSubtitleManager()->DisplaySubtitles(SceneCanvas, SubtitleRegion, MyWorld->GetAudioTimeSeconds());
							bDisplayedSubtitles = true;
						}
					}
				}
			}
		}

		//ensure canvas has been flushed before rendering UI
		SceneCanvas->Flush_GameThread();

		// Allow the viewport to render additional stuff
		PostRender(DebugCanvasObject);
	}


	// Grab the player camera location and orientation so we can pass that along to the stats drawing code.
	FVector PlayerCameraLocation = FVector::ZeroVector;
	FRotator PlayerCameraRotation = FRotator::ZeroRotator;
	{
		for (FConstPlayerControllerIterator Iterator = MyWorld->GetPlayerControllerIterator(); Iterator; ++Iterator)
		{
			if (APlayerController* PC = Iterator->Get())
			{
				PC->GetPlayerViewPoint(PlayerCameraLocation, PlayerCameraRotation);
			}
		}
	}

	if (DebugCanvas)
	{
		// Reset the debug canvas to be full-screen before drawing the console
		// (the debug draw service above has messed with the viewport size to fit it to a single player's subregion)
		DebugCanvasObject->Init(DebugCanvasSize.X, DebugCanvasSize.Y, NULL, DebugCanvas);

		DrawStatsHUD(MyWorld, InViewport, DebugCanvas, DebugCanvasObject, DebugProperties, PlayerCameraLocation, PlayerCameraRotation);

		if (GEngine->IsStereoscopic3D(InViewport))
		{
#if 0 //!UE_BUILD_SHIPPING
			// TODO: replace implementation in OculusHMD with a debug renderer
			if (GEngine->XRSystem.IsValid())
			{
				GEngine->XRSystem->DrawDebug(DebugCanvasObject);
			}
#endif
		}

		// Render the console absolutely last because developer input is was matter the most.
		if (ViewportConsole)
		{
			ViewportConsole->PostRender_Console(DebugCanvasObject);
		}
	}

	FSimpleMulticastDelegate& DrawDelegateEnd = OnEndDraw();
	DrawDelegateEnd.Broadcast();
}