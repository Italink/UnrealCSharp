#include "FCSharpCompilerRunnable.h"
#include "Common/FUnrealCSharpFunctionLibrary.h"
#include "Delegate/FUnrealCSharpCoreModuleDelegates.h"
#include "Dynamic/FDynamicGenerator.h"
#include "Log/UnrealCSharpLog.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Setting/UnrealCSharpEditorSetting.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "UEVersion.h"
#include "FCSharpCompiler.h"
#if UE_APP_STYLE_GET_BRUSH
#include "Styling/AppStyle.h"
#else
#include "EditorStyleSet.h"
#endif

TSharedPtr<SNotificationItem> FCSharpCompilerRunnable::CompileStateNotification;

FCSharpCompilerRunnable::FCSharpCompilerRunnable():
	Event(nullptr),
	bIsCompiling(false),
	bIsGenerating(false),
	bIsStopped(false)
{
	OnBeginGeneratorDelegateHandle = FUnrealCSharpCoreModuleDelegates::OnBeginGenerator.AddRaw(
		this, &FCSharpCompilerRunnable::OnBeginGenerator);

	OnEndGeneratorDelegateHandle = FUnrealCSharpCoreModuleDelegates::OnEndGenerator.AddRaw(
		this, &FCSharpCompilerRunnable::OnEndGenerator);
}

FCSharpCompilerRunnable::~FCSharpCompilerRunnable()
{
	if (OnEndGeneratorDelegateHandle.IsValid())
	{
		FUnrealCSharpCoreModuleDelegates::OnEndGenerator.Remove(OnEndGeneratorDelegateHandle);
	}

	if (OnBeginGeneratorDelegateHandle.IsValid())
	{
		FUnrealCSharpCoreModuleDelegates::OnBeginGenerator.Remove(OnBeginGeneratorDelegateHandle);
	}
}

bool FCSharpCompilerRunnable::Init()
{
	Event = FPlatformProcess::GetSynchEventFromPool(true);

	return FRunnable::Init();
}

uint32 FCSharpCompilerRunnable::Run()
{
	while (true)
	{
		if (bIsStopped)
		{
			return 0;
		}

		if (!bIsGenerating)
		{
			if (!Tasks.IsEmpty())
			{
				bool Task = false;

				{
					FScopeLock ScopeLock(&CriticalSection);

					if (!Tasks.IsEmpty())
					{
						Tasks.Dequeue(Task);
					}
				}

				if (Task == true)
				{
					DoWork();
				}
			}
			else
			{
				if (Event != nullptr)
				{
					Event->Wait();
				}
			}
		}
	}
}

void FCSharpCompilerRunnable::Stop()
{
	bIsStopped = true;

	if (Event != nullptr)
	{
		Event->Trigger();
	}
}

void FCSharpCompilerRunnable::Exit()
{
	if (Event != nullptr)
	{
		FPlatformProcess::ReturnSynchEventToPool(Event);

		Event = nullptr;
	}
}

void FCSharpCompilerRunnable::EnqueueTask()
{
	{
		FScopeLock ScopeLock(&CriticalSection);

		if (!Tasks.IsEmpty())
		{
			Tasks.Empty();
		}

		Tasks.Enqueue(true);
	}

	Event->Trigger();
}

void FCSharpCompilerRunnable::EnqueueTask(const TArray<FFileChangeData>& InFileChangeData)
{
	{
		FScopeLock ScopeLock(&CriticalSection);

		if (!Tasks.IsEmpty())
		{
			Tasks.Empty();
		}

		FileChanges.Append(InFileChangeData);

		Tasks.Enqueue(true);
	}

	Event->Trigger();
}

bool FCSharpCompilerRunnable::IsCompiling() const
{
	return bIsCompiling == true || !Tasks.IsEmpty();
}

void FCSharpCompilerRunnable::DoWork()
{
	Compile([&]()
	{
		FDynamicGenerator::Generator(FileChanges);

		FileChanges.Empty();
	});
}

void FCSharpCompilerRunnable::ImmediatelyDoWork()
{
	Compile([]()
	{
		FDynamicGenerator::Generator();
	});
}

void FCSharpCompilerRunnable::Compile(const TFunction<void()>& InFunction)
{
	if (const auto UnrealCSharpEditorSetting = FUnrealCSharpFunctionLibrary::GetMutableDefaultSafe<
		UUnrealCSharpEditorSetting>())
	{
		if (UnrealCSharpEditorSetting->EnableCompiled())
		{
			bIsCompiling = true;

			Compile();

			const auto Task = FFunctionGraphTask::CreateAndDispatchWhenReady(
				[InFunction, this]()
				{
					if (!GExitPurge)
					{
						FUnrealCSharpCoreModuleDelegates::OnCompile.Broadcast(FileChanges);

						InFunction();
					}
				},
				TStatId(),
				nullptr,
				ENamedThreads::GameThread);

			FTaskGraphInterface::Get().WaitUntilTaskCompletes(Task);

			bIsCompiling = false;
		}
	}
}

void FCSharpCompilerRunnable::Compile()
{
	if (CompileStateNotification && CompileStateNotification->GetCompletionState() != SNotificationItem::ECompletionState::CS_Pending)
	{
		CompileStateNotification->Fadeout();
		CompileStateNotification.Reset();
	}
	if (CompileStateNotification == nullptr)
	{
		AsyncTask(ENamedThreads::GameThread, []()
			{
				FNotificationInfo Info(FText::FromString("Unreal CSharp"));
				Info.SubText = FText::FromString("Compiling");
				Info.bFireAndForget = false;
				Info.bUseThrobber = true;
				Info.bUseSuccessFailIcons = true;
				Info.FadeOutDuration = 2.0f;
				Info.bUseLargeFont = true;
				Info.ButtonDetails.Add(FNotificationButtonInfo(
					FText::FromString("Recompile"),
					FText::FromString("Recompile"),
					FSimpleDelegate::CreateLambda([]()
						{
							if (CompileStateNotification)
							{
								CompileStateNotification->Fadeout();
								CompileStateNotification.Reset();
							}
							FCSharpCompiler::Get().Compile();
						}),
					SNotificationItem::ECompletionState::CS_Fail
				));
				Info.ButtonDetails.Add(FNotificationButtonInfo(
					FText::FromString("Ignore"),
					FText::FromString("Ignore"),
					FSimpleDelegate::CreateLambda([]()
						{
							if (CompileStateNotification.IsValid())
							{
								CompileStateNotification->Fadeout();
								CompileStateNotification.Reset();
							}
						}),
					SNotificationItem::ECompletionState::CS_Fail
				));
				CompileStateNotification = FSlateNotificationManager::Get().AddNotification(Info);
				if (CompileStateNotification)
				{
					CompileStateNotification->SetCompletionState(SNotificationItem::CS_Pending);
				}
			});
	}

	static auto CompileTool = FUnrealCSharpFunctionLibrary::GetDotNet();

	const auto CompileParam = FString::Printf(TEXT(
		"publish \"%s\" --nologo -c Debug -o \"%s\""
	),
		*FUnrealCSharpFunctionLibrary::GetGameProjectPath(),
		*FUnrealCSharpFunctionLibrary::GetFullPublishDirectory()
	);

	void* ReadPipe = nullptr;

	void* WritePipe = nullptr;

	auto OutProcessID = 0u;

	FString Result;

	FPlatformProcess::CreatePipe(ReadPipe, WritePipe);

	auto ProcessHandle = FPlatformProcess::CreateProc(
		*CompileTool,
		*CompileParam,
		false,
		true,
		true,
		&OutProcessID,
		1,
		nullptr,
		WritePipe,
		ReadPipe);

	while (ProcessHandle.IsValid() && FPlatformProcess::IsApplicationRunning(OutProcessID))
	{
		FPlatformProcess::Sleep(0.01f);

		Result.Append(FPlatformProcess::ReadPipe(ReadPipe));
	}

	auto ReturnCode = 0;

	if (FPlatformProcess::GetProcReturnCode(ProcessHandle, &ReturnCode))
	{
		if (CompileStateNotification) {
			AsyncTask(ENamedThreads::GameThread, [ReturnCode, Result]()
				{
					if (!CompileStateNotification || !FSlateApplication::IsInitialized())
					{
						UE_LOG(LogUnrealCSharp, Error, TEXT("CompileStateNotification is invalid"));
						return;
					}
					if (ReturnCode == 0)
					{
						CompileStateNotification->SetCompletionState(SNotificationItem::CS_Success);
						CompileStateNotification->SetSubText(FText::FromString(TEXT("Compilation succeeded")));
						CompileStateNotification->Fadeout();
						CompileStateNotification.Reset();
						UE_LOG(LogUnrealCSharp, Log, TEXT("Compilation succeeded"));
					}
					else
					{
						CompileStateNotification->SetSubText(FText::FromString(TEXT("Compilation failed")));
						CompileStateNotification->SetCompletionState(SNotificationItem::CS_Fail);
						UE_LOG(LogUnrealCSharp, Error, TEXT("%s"), *Result);
					}
				});
		}
	}

	FPlatformProcess::ClosePipe(ReadPipe, WritePipe);

	FPlatformProcess::CloseProc(ProcessHandle);
}

void FCSharpCompilerRunnable::OnBeginGenerator()
{
	bIsGenerating = true;

	Tasks.Empty();

	FileChanges.Empty();
}

void FCSharpCompilerRunnable::OnEndGenerator()
{
	bIsGenerating = false;

	Tasks.Empty();

	FileChanges.Empty();
}
