// Copyright Ilgar Lunin. All Rights Reserved.

#include "VoskComponent.h"
#include "Voice.h"
#include "VoskSoundUtils.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"


// Sets default values for this component's properties
UVoskComponent::UVoskComponent()
{
	// Set this component to be initialized when the game starts, and to be ticked every frame.  You can turn these features
	// off to improve performance if you don't need them.
	PrimaryComponentTick.bCanEverTick = true;
    _final_result_request_message = TEXT("{\"eof\" : 1}");
}


bool UVoskComponent::BeginCapture()
{
    if (bIsCaptureActive) return false;

    if (!FVoiceModule::Get().DoesPlatformSupportVoiceCapture())
    {
        UE_LOG(LogTemp, Log, TEXT("%s"), TEXT("VoiceCapture is not supported on this platform!"));
    }

    if (!_voice_capture.IsValid())
    {
        _voice_capture = FVoiceModule::Get().CreateVoiceCapture("");
        if (_voice_capture.IsValid())
        {
            FString DeviceName;
            if (!_voice_capture->Init(DeviceName, _sample_rate, 1))
                return false;
            UE_LOG(LogTemp, Log, TEXT("IVoiceCapture initialized"));
        }
        else
        {
            UE_LOG(LogTemp, Error, TEXT("Failed to obtain IVoiceCapture, no voice available!"));
            return false;
        }
    }

    UE_LOG(LogTemp, Log, TEXT("Capture started"));
    _recorded_samples.Empty();
    _voice_capture->Start();
    bIsCaptureActive = true;

    return true;
}

// Called every frame
void UVoskComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (_voice_capture.IsValid() && bIsCaptureActive)
    {
        uint32 VoiceCaptureBytesAvailable = 0;
        EVoiceCaptureState::Type CaptureState = _voice_capture->GetCaptureState(VoiceCaptureBytesAvailable);

        if (CaptureState == EVoiceCaptureState::Ok && VoiceCaptureBytesAvailable > 0)
        {
            uint32 VoiceCaptureReadBytes = 0;

            TArray<uint8> _recorded_chunk;
            uint32 record_start = _recorded_chunk.AddUninitialized(VoiceCaptureBytesAvailable);
            
            EVoiceCaptureState::Type microphone_state = _voice_capture->GetVoiceData(_recorded_chunk.GetData(), VoiceCaptureBytesAvailable, VoiceCaptureReadBytes);
            if (microphone_state == EVoiceCaptureState::Ok)
            {
                if (VoiceCaptureReadBytes > 0)
                {
                    _recorded_samples += _recorded_chunk;
                    if (IsInitialized())
                        Socket->Send(_recorded_chunk.GetData(), VoiceCaptureReadBytes, true);
                }
            }
        }
    }
}

void UVoskComponent::DecodeRresult(const FString& raw)
{
    TSharedPtr<FJsonObject> result;
    TSharedRef<TJsonReader<>> reader = TJsonReaderFactory<>::Create(raw);

    if (FJsonSerializer::Deserialize(reader, result))
    {
        FString _res;
        if (result->TryGetStringField(TEXT("partial"), _res))
        {
            _res_partial = _res;
            OnPartialResultReceived.Broadcast(_res_partial);
        }
        else if (result->TryGetStringField(TEXT("text"), _res))
        {
            _res_final = _res;
            OnFinalResultReceived.Broadcast(_res_final);
        }
    }
}

void UVoskComponent::FinishCapture(TArray<uint8> &CaptureData, int32 &SamplesRecorded)
{
    bIsCaptureActive = false;

    SamplesRecorded = _recorded_samples.Num();
    CaptureData = _recorded_samples;

    if (_voice_capture.IsValid())
        _voice_capture->Stop();
}

bool UVoskComponent::SendVoiceDataToLanguageServer(const TArray<uint8>& VoiceChunk, int32 PacketSize)
{
    if (!IsInitialized())
    {
        UE_LOG(LogTemp, Warning, TEXT("Socket is not initialized!"));
        return false;
    }

    PacketSize = FMath::Clamp<int32>(PacketSize, 0, TNumericLimits<int32>::Max());

    if (VoiceChunk.Num() < PacketSize)
    {
        UE_LOG(LogTemp, Warning, TEXT("Voice chunk is too small"));
        return false;
    }

    const int32 NumPackets = VoiceChunk.Num() / PacketSize;
    size_t BytesSent = 0;
    for (int i = 0; i < NumPackets; i++)
    {
        Socket->Send(VoiceChunk.GetData() + (i * PacketSize), PacketSize, true);
        BytesSent += PacketSize;
    }

    if (BytesSent < (size_t)VoiceChunk.Num())
    {
        // send remainder
        const size_t remainder = VoiceChunk.Num() - BytesSent;
        Socket->Send(VoiceChunk.GetData() + BytesSent, remainder, true);
        BytesSent += remainder;
    }

    return VoiceChunk.Num() == BytesSent;
}


void UVoskComponent::Initialize(FString Addr, int32 Port)
{
    if (Addr.ToLower().Equals(TEXT("localhost")))
        Addr = TEXT("127.0.0.1");

    const FString Protocol("ws");
    const FString ServerURL = FString::Printf(TEXT("%s://%s:%d/"), *Protocol, *Addr, Port);  // Server URL. You can use ws, wss or wss+insecure.
    Socket = FWebSocketsModule::Get().CreateWebSocket(ServerURL, Protocol);

    Socket->OnConnected().AddLambda([this]() {
        OnConnectedToServer.Broadcast();
    });

    Socket->OnConnectionError().AddLambda([&](const FString &error) {
        OnConnectionError.Broadcast(error);
    });

    Socket->OnClosed().AddLambda([this](int32 StatusCode, const FString& Reason, bool bWasClean) -> void {
        // This code will run when the connection to the server has been terminated.
        // Because of an error or a call to Socket->Close().
        OnConnectionTerminated.Broadcast(StatusCode, Reason, bWasClean);
    });

    Socket->OnMessage().AddLambda([this](const FString& Message) -> void {
        // try to decode partial result
        DecodeRresult(Message);
    });

    Socket->OnRawMessage().AddLambda([](const void* Data, SIZE_T Size, SIZE_T BytesRemaining) -> void {
        // This code will run when we receive a raw (binary) message from the server.
        // UE_LOG(LogTemp, Warning, TEXT("Binary data received: %d - %d"), Size, BytesRemaining);

    });

    Socket->OnMessageSent().AddLambda([](const FString& MessageString) -> void {
        // This code is called after we sent a message to the server.
    });

    // And we finally connect to the server. 
    Socket->Connect();
}

bool UVoskComponent::IsInitialized()
{
    if (!Socket.IsValid())
    {
        return false;
    }

    return Socket->IsConnected();
}

void UVoskComponent::Uninitialize()
{
    if (_voice_capture.IsValid() && bIsCaptureActive)
    {
        _voice_capture->Stop();
        bIsCaptureActive = false;
    }

    if (IsInitialized())
    {
        // disconnect delegates
        Socket->OnConnected().Clear();
        Socket->OnConnectionError().Clear();
        Socket->OnClosed().Clear();
        Socket->OnMessage().Clear();
        Socket->OnRawMessage().Clear();
        Socket->OnMessageSent().Clear();
        Socket->Close(0, TEXT("End Play"));
    }
}

FString UVoskComponent::GetPartialResult()
{
    return _res_partial;
}

void UVoskComponent::ResetPartialResult()
{
    _res_partial = "";
}

void UVoskComponent::ResetFinalResult()
{
    _res_final = "";
}

void UVoskComponent::RequestFinalResult()
{
    if (IsInitialized())
    {
        // tell server to send final result
        Socket->Send(_final_result_request_message);
    }
}

void UVoskComponent::SetFinalResultRequestMessage(const FString& message)
{
    _final_result_request_message = message;
}

FString UVoskComponent::GetFinalResult()
{
    return _res_final;
}

USoundWave* UVoskComponent::SamplesToSound(
    const TArray<uint8>& samples,
    int32 SampleRate,
    int32 NumChannels)
{
    return VoskComponentUtils::CreateSoundFromWaveDataWithoutHeader(
        samples,
        SampleRate,
        NumChannels);
}

int32 UVoskComponent::GetSoundSampleRate(USoundWave* SoundWave)
{
    return VoskComponentUtils::GetSoundSampleRate(SoundWave);
}

void UVoskComponent::CreateProcess(FProcessHandleWrapper& ProcessHandle,
                                    FString FullPathOfProgramToRun,
                                    TArray<FString> CommandlineArgs,
                                    bool Detach,
                                    bool Hidden, int32 Priority)
{
    FString Args = "";
    if (CommandlineArgs.Num() > 1)
    {
        Args = CommandlineArgs[0];
        for (int32 v = 1; v < CommandlineArgs.Num(); v++)
        {
            Args += " " + CommandlineArgs[v];
        }
    }
    else if (CommandlineArgs.Num() > 0)
    {
        Args = CommandlineArgs[0];
    }

    ProcessHandle.handle = FPlatformProcess::CreateProc(
        *FullPathOfProgramToRun,
        *Args,
        Detach,                                 //  if true, the new process will have its own window
        false,                                  //  if true, the new process will be minimized in the task bar
        Hidden,                                 //  if true, the new process will not have a window or be in the task bar
        &ProcessHandle.processId,
        Priority,
        nullptr,
        nullptr
    );
}

void UVoskComponent::KillProcess(const FProcessHandleWrapper& processHandle)
{
    FPlatformProcess::TerminateProc((const_cast<FProcessHandleWrapper&>(processHandle)).handle, true);
}

TArray<FString> UVoskComponent::BuildServerParameters(FVoskServerParameters params, bool& Success)
{
    TArray<FString> out;
    out.Add("--address"); out.Add(params.Address);
    out.Add("--port"); out.Add(FString::Printf(TEXT("%d"), params.Port));
    out.Add("--threads"); out.Add(FString::Printf(TEXT("%d"), params.Threads));
    out.Add("--sample-rate"); out.Add(FString::Printf(TEXT("%d"), params.SampleRate));
    out.Add("--show-words"); out.Add(FString::Printf(TEXT("%d"), params.ShowWords));
    out.Add("--model-path"); out.Add(params.PathToModel);

    Success = FPaths::DirectoryExists(params.PathToModel) && !params.PathToModel.IsEmpty();

    return out;
}

FString UVoskComponent::GetProcessExecutablePath()
{
    return FPaths::GetPath(FPlatformProcess::ExecutablePath());
}

void UVoskComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    Uninitialize();
}
