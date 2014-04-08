/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "AudioBufferSourceNode.h"
#include "mozilla/dom/AudioBufferSourceNodeBinding.h"
#include "mozilla/dom/AudioParam.h"
#include "nsMathUtils.h"
#include "AudioNodeEngine.h"
#include "AudioNodeStream.h"
#include "AudioDestinationNode.h"
#include "AudioParamTimeline.h"
#include "speex/speex_resampler.h"
#include <limits>

namespace mozilla {
namespace dom {

NS_IMPL_CYCLE_COLLECTION_CLASS(AudioBufferSourceNode)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(AudioBufferSourceNode)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mBuffer)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mPlaybackRate)
  if (tmp->Context()) {
    // AudioNode's Unlink implementation disconnects us from the graph
    // too, but we need to do this right here to make sure that
    // UnregisterAudioBufferSourceNode can properly untangle us from
    // the possibly connected PannerNodes.
    tmp->DisconnectFromGraph();
    tmp->Context()->UnregisterAudioBufferSourceNode(tmp);
  }
NS_IMPL_CYCLE_COLLECTION_UNLINK_END_INHERITED(AudioNode)

NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN_INHERITED(AudioBufferSourceNode, AudioNode)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mBuffer)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mPlaybackRate)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION_INHERITED(AudioBufferSourceNode)
NS_INTERFACE_MAP_END_INHERITING(AudioNode)

NS_IMPL_ADDREF_INHERITED(AudioBufferSourceNode, AudioNode)
NS_IMPL_RELEASE_INHERITED(AudioBufferSourceNode, AudioNode)

/**
 * Media-thread playback engine for AudioBufferSourceNode.
 * Nothing is played until a non-null buffer has been set (via
 * AudioNodeStream::SetBuffer) and a non-zero mBufferEnd has been set (via
 * AudioNodeStream::SetInt32Parameter).
 */
class AudioBufferSourceNodeEngine : public AudioNodeEngine
{
public:
  explicit AudioBufferSourceNodeEngine(AudioNode* aNode,
                                       AudioDestinationNode* aDestination) :
    AudioNodeEngine(aNode),
    mStart(0.0), mBeginProcessing(0),
    mStop(TRACK_TICKS_MAX),
    mResampler(nullptr), mRemainingResamplerTail(0),
    mBufferEnd(0),
    mLoopStart(0), mLoopEnd(0),
    mBufferSampleRate(0), mBufferPosition(0), mChannels(0),
    mDopplerShift(1.0f),
    mDestination(static_cast<AudioNodeStream*>(aDestination->Stream())),
    mPlaybackRateTimeline(1.0f), mLoop(false)
  {}

  ~AudioBufferSourceNodeEngine()
  {
    if (mResampler) {
      speex_resampler_destroy(mResampler);
    }
  }

  void SetSourceStream(AudioNodeStream* aSource)
  {
    mSource = aSource;
  }

  virtual void SetTimelineParameter(uint32_t aIndex,
                                    const dom::AudioParamTimeline& aValue,
                                    TrackRate aSampleRate) MOZ_OVERRIDE
  {
    switch (aIndex) {
    case AudioBufferSourceNode::PLAYBACKRATE:
      mPlaybackRateTimeline = aValue;
      WebAudioUtils::ConvertAudioParamToTicks(mPlaybackRateTimeline, mSource, mDestination);
      break;
    default:
      NS_ERROR("Bad AudioBufferSourceNodeEngine TimelineParameter");
    }
  }
  virtual void SetStreamTimeParameter(uint32_t aIndex, TrackTicks aParam)
  {
    switch (aIndex) {
    case AudioBufferSourceNode::STOP: mStop = aParam; break;
    default:
      NS_ERROR("Bad AudioBufferSourceNodeEngine StreamTimeParameter");
    }
  }
  virtual void SetDoubleParameter(uint32_t aIndex, double aParam)
  {
    switch (aIndex) {
    case AudioBufferSourceNode::START:
      MOZ_ASSERT(!mStart, "Another START?");
      mStart = mSource->TimeFromDestinationTime(mDestination, aParam) *
        mSource->SampleRate();
      // Round to nearest
      mBeginProcessing = mStart + 0.5;
      break;
    case AudioBufferSourceNode::DOPPLERSHIFT:
      mDopplerShift = aParam > 0 && aParam == aParam ? aParam : 1.0;
      break;
    default:
      NS_ERROR("Bad AudioBufferSourceNodeEngine double parameter.");
    };
  }
  virtual void SetInt32Parameter(uint32_t aIndex, int32_t aParam)
  {
    switch (aIndex) {
    case AudioBufferSourceNode::SAMPLE_RATE: mBufferSampleRate = aParam; break;
    case AudioBufferSourceNode::BUFFERSTART:
      if (mBufferPosition == 0) {
        mBufferPosition = aParam;
      }
      break;
    case AudioBufferSourceNode::BUFFEREND: mBufferEnd = aParam; break;
    case AudioBufferSourceNode::LOOP: mLoop = !!aParam; break;
    case AudioBufferSourceNode::LOOPSTART: mLoopStart = aParam; break;
    case AudioBufferSourceNode::LOOPEND: mLoopEnd = aParam; break;
    default:
      NS_ERROR("Bad AudioBufferSourceNodeEngine Int32Parameter");
    }
  }
  virtual void SetBuffer(already_AddRefed<ThreadSharedFloatArrayBufferList> aBuffer)
  {
    mBuffer = aBuffer;
  }

  bool BegunResampling()
  {
    return mBeginProcessing == -TRACK_TICKS_MAX;
  }

  void UpdateResampler(int32_t aOutRate, uint32_t aChannels)
  {
    if (mResampler &&
        (aChannels != mChannels ||
         // If the resampler has begun, then it will have moved
         // mBufferPosition to after the samples it has read, but it hasn't
         // output its buffered samples.  Keep using the resampler, even if
         // the rates now match, so that this latent segment is output.
         (aOutRate == mBufferSampleRate && !BegunResampling()))) {
      speex_resampler_destroy(mResampler);
      mResampler = nullptr;
      mBeginProcessing = mStart + 0.5;
    }

    if (aOutRate == mBufferSampleRate && !mResampler) {
      return;
    }

    if (!mResampler) {
      mChannels = aChannels;
      mResampler = speex_resampler_init(mChannels, mBufferSampleRate, aOutRate,
                                        SPEEX_RESAMPLER_QUALITY_DEFAULT,
                                        nullptr);
    } else {
      uint32_t currentOutSampleRate, currentInSampleRate;
      speex_resampler_get_rate(mResampler, &currentInSampleRate,
                               &currentOutSampleRate);
      if (currentOutSampleRate == static_cast<uint32_t>(aOutRate)) {
        return;
      }
      speex_resampler_set_rate(mResampler, currentInSampleRate, aOutRate);
    }

    if (!BegunResampling()) {
      // Low pass filter effects from the resampler mean that samples before
      // the start time are influenced by resampling the buffer.  The input
      // latency indicates half the filter width.
      int64_t inputLatency = speex_resampler_get_input_latency(mResampler);
      uint32_t ratioNum, ratioDen;
      speex_resampler_get_ratio(mResampler, &ratioNum, &ratioDen);
      // The output subsample resolution supported in aligning the resampler
      // is ratioNum.  First round the start time to the nearest subsample.
      int64_t subsample = mStart * ratioNum + 0.5;
      // Now include the leading effects of the filter, and round *up* to the
      // next whole tick, because there is no effect on samples outside the
      // filter width.
      mBeginProcessing =
        (subsample - inputLatency * ratioDen + ratioNum - 1) / ratioNum;
    }
  }

  // Borrow a full buffer of size WEBAUDIO_BLOCK_SIZE from the source buffer
  // at offset aSourceOffset.  This avoids copying memory.
  void BorrowFromInputBuffer(AudioChunk* aOutput,
                             uint32_t aChannels)
  {
    aOutput->mDuration = WEBAUDIO_BLOCK_SIZE;
    aOutput->mBuffer = mBuffer;
    aOutput->mChannelData.SetLength(aChannels);
    for (uint32_t i = 0; i < aChannels; ++i) {
      aOutput->mChannelData[i] = mBuffer->GetData(i) + mBufferPosition;
    }
    aOutput->mVolume = 1.0f;
    aOutput->mBufferFormat = AUDIO_FORMAT_FLOAT32;
  }

  // Copy aNumberOfFrames frames from the source buffer at offset aSourceOffset
  // and put it at offset aBufferOffset in the destination buffer.
  void CopyFromInputBuffer(AudioChunk* aOutput,
                           uint32_t aChannels,
                           uintptr_t aOffsetWithinBlock,
                           uint32_t aNumberOfFrames) {
    for (uint32_t i = 0; i < aChannels; ++i) {
      float* baseChannelData = static_cast<float*>(const_cast<void*>(aOutput->mChannelData[i]));
      memcpy(baseChannelData + aOffsetWithinBlock,
             mBuffer->GetData(i) + mBufferPosition,
             aNumberOfFrames * sizeof(float));
    }
  }

  // Resamples input data to an output buffer, according to |mBufferSampleRate| and
  // the playbackRate.
  // The number of frames consumed/produced depends on the amount of space
  // remaining in both the input and output buffer, and the playback rate (that
  // is, the ratio between the output samplerate and the input samplerate).
  void CopyFromInputBufferWithResampling(AudioNodeStream* aStream,
                                         AudioChunk* aOutput,
                                         uint32_t aChannels,
                                         uint32_t* aOffsetWithinBlock,
                                         TrackTicks* aCurrentPosition,
                                         int32_t aBufferMax) {
    // TODO: adjust for mStop (see bug 913854 comment 9).
    uint32_t availableInOutputBuffer =
      WEBAUDIO_BLOCK_SIZE - *aOffsetWithinBlock;
    SpeexResamplerState* resampler = mResampler;
    MOZ_ASSERT(aChannels > 0);

    if (mBufferPosition < aBufferMax) {
      uint32_t availableInInputBuffer = aBufferMax - mBufferPosition;
      uint32_t ratioNum, ratioDen;
      speex_resampler_get_ratio(resampler, &ratioNum, &ratioDen);
      // Limit the number of input samples copied and possibly
      // format-converted for resampling by estimating how many will be used.
      // This may be a little small if still filling the resampler with
      // initial data, but we'll get called again and it will work out.
      uint32_t inputLimit = availableInOutputBuffer * ratioNum / ratioDen + 10;
      if (!BegunResampling()) {
        // First time the resampler is used.
        uint32_t inputLatency = speex_resampler_get_input_latency(resampler);
        inputLimit += inputLatency;
        // If starting after mStart, then play from the beginning of the
        // buffer, but correct for input latency.  If starting before mStart,
        // then align the resampler so that the time corresponding to the
        // first input sample is mStart.
        uint32_t skipFracNum = inputLatency * ratioDen;
        double leadTicks = mStart - *aCurrentPosition;
        if (leadTicks > 0.0) {
          // Round to nearest output subsample supported by the resampler at
          // these rates.
          skipFracNum -= leadTicks * ratioNum + 0.5;
          MOZ_ASSERT(skipFracNum < INT32_MAX, "mBeginProcessing is wrong?");
        }
        speex_resampler_set_skip_frac_num(resampler, skipFracNum);

        mBeginProcessing = -TRACK_TICKS_MAX;
      }
      inputLimit = std::min(inputLimit, availableInInputBuffer);

      for (uint32_t i = 0; true; ) {
        uint32_t inSamples = inputLimit;
        const float* inputData = mBuffer->GetData(i) + mBufferPosition;

        uint32_t outSamples = availableInOutputBuffer;
        float* outputData =
          static_cast<float*>(const_cast<void*>(aOutput->mChannelData[i])) +
          *aOffsetWithinBlock;

        WebAudioUtils::SpeexResamplerProcess(resampler, i,
                                             inputData, &inSamples,
                                             outputData, &outSamples);
        if (++i == aChannels) {
          mBufferPosition += inSamples;
          MOZ_ASSERT(mBufferPosition <= mBufferEnd || mLoop);
          *aOffsetWithinBlock += outSamples;
          *aCurrentPosition += outSamples;
          if (inSamples == availableInInputBuffer && !mLoop) {
            // We'll feed in enough zeros to empty out the resampler's memory.
            // This handles the output latency as well as capturing the low
            // pass effects of the resample filter.
            mRemainingResamplerTail =
              2 * speex_resampler_get_input_latency(resampler) - 1;
          }
          return;
        }
      }
    } else {
      for (uint32_t i = 0; true; ) {
        uint32_t inSamples = mRemainingResamplerTail;
        uint32_t outSamples = availableInOutputBuffer;
        float* outputData =
          static_cast<float*>(const_cast<void*>(aOutput->mChannelData[i])) +
          *aOffsetWithinBlock;

        // AudioDataValue* for aIn selects the function that does not try to
        // copy and format-convert input data.
        WebAudioUtils::SpeexResamplerProcess(resampler, i,
                         static_cast<AudioDataValue*>(nullptr), &inSamples,
                         outputData, &outSamples);
        if (++i == aChannels) {
          mRemainingResamplerTail -= inSamples;
          MOZ_ASSERT(mRemainingResamplerTail >= 0);
          *aOffsetWithinBlock += outSamples;
          *aCurrentPosition += outSamples;
          break;
        }
      }
    }
  }

  /**
   * Fill aOutput with as many zero frames as we can, and advance
   * aOffsetWithinBlock and aCurrentPosition based on how many frames we write.
   * This will never advance aOffsetWithinBlock past WEBAUDIO_BLOCK_SIZE or
   * aCurrentPosition past aMaxPos.  This function knows when it needs to
   * allocate the output buffer, and also optimizes the case where it can avoid
   * memory allocations.
   */
  void FillWithZeroes(AudioChunk* aOutput,
                      uint32_t aChannels,
                      uint32_t* aOffsetWithinBlock,
                      TrackTicks* aCurrentPosition,
                      TrackTicks aMaxPos)
  {
    MOZ_ASSERT(*aCurrentPosition < aMaxPos);
    uint32_t numFrames =
      std::min<TrackTicks>(WEBAUDIO_BLOCK_SIZE - *aOffsetWithinBlock,
                           aMaxPos - *aCurrentPosition);
    if (numFrames == WEBAUDIO_BLOCK_SIZE) {
      aOutput->SetNull(numFrames);
    } else {
      if (*aOffsetWithinBlock == 0) {
        AllocateAudioBlock(aChannels, aOutput);
      }
      WriteZeroesToAudioBlock(aOutput, *aOffsetWithinBlock, numFrames);
    }
    *aOffsetWithinBlock += numFrames;
    *aCurrentPosition += numFrames;
  }

  /**
   * Copy as many frames as possible from the source buffer to aOutput, and
   * advance aOffsetWithinBlock and aCurrentPosition based on how many frames
   * we write.  This will never advance aOffsetWithinBlock past
   * WEBAUDIO_BLOCK_SIZE, or aCurrentPosition past mStop.  It takes data from
   * the buffer at aBufferOffset, and never takes more data than aBufferMax.
   * This function knows when it needs to allocate the output buffer, and also
   * optimizes the case where it can avoid memory allocations.
   */
  void CopyFromBuffer(AudioNodeStream* aStream,
                      AudioChunk* aOutput,
                      uint32_t aChannels,
                      uint32_t* aOffsetWithinBlock,
                      TrackTicks* aCurrentPosition,
                      int32_t aBufferMax)
  {
    MOZ_ASSERT(*aCurrentPosition < mStop);
    uint32_t numFrames =
      std::min(std::min<TrackTicks>(WEBAUDIO_BLOCK_SIZE - *aOffsetWithinBlock,
                                    aBufferMax - mBufferPosition),
               mStop - *aCurrentPosition);
    if (numFrames == WEBAUDIO_BLOCK_SIZE && !mResampler) {
      MOZ_ASSERT(mBufferPosition < aBufferMax);
      BorrowFromInputBuffer(aOutput, aChannels);
      *aOffsetWithinBlock += numFrames;
      *aCurrentPosition += numFrames;
      mBufferPosition += numFrames;
    } else {
      if (*aOffsetWithinBlock == 0) {
        AllocateAudioBlock(aChannels, aOutput);
      }
      if (!mResampler) {
        MOZ_ASSERT(mBufferPosition < aBufferMax);
        CopyFromInputBuffer(aOutput, aChannels, *aOffsetWithinBlock, numFrames);
        *aOffsetWithinBlock += numFrames;
        *aCurrentPosition += numFrames;
        mBufferPosition += numFrames;
      } else {
        CopyFromInputBufferWithResampling(aStream, aOutput, aChannels, aOffsetWithinBlock, aCurrentPosition, aBufferMax);
      }
    }
  }

  int32_t ComputeFinalOutSampleRate(float aPlaybackRate)
  {
    // Make sure the playback rate and the doppler shift are something
    // our resampler can work with.
    int32_t rate = WebAudioUtils::
      TruncateFloatToInt<int32_t>(mSource->SampleRate() /
                                  (aPlaybackRate * mDopplerShift));
    return rate ? rate : mBufferSampleRate;
  }

  void UpdateSampleRateIfNeeded(uint32_t aChannels)
  {
    float playbackRate;

    if (mPlaybackRateTimeline.HasSimpleValue()) {
      playbackRate = mPlaybackRateTimeline.GetValue();
    } else {
      playbackRate = mPlaybackRateTimeline.GetValueAtTime(mSource->GetCurrentPosition());
    }
    if (playbackRate <= 0 || playbackRate != playbackRate) {
      playbackRate = 1.0f;
    }

    int32_t outRate = ComputeFinalOutSampleRate(playbackRate);
    UpdateResampler(outRate, aChannels);
  }

  virtual void ProcessBlock(AudioNodeStream* aStream,
                            const AudioChunk& aInput,
                            AudioChunk* aOutput,
                            bool* aFinished)
  {
    if (!mBuffer || !mBufferEnd) {
      aOutput->SetNull(WEBAUDIO_BLOCK_SIZE);
      return;
    }

    uint32_t channels = mBuffer->GetChannels();
    if (!channels) {
      aOutput->SetNull(WEBAUDIO_BLOCK_SIZE);
      return;
    }

    // WebKit treats the playbackRate as a k-rate parameter in their code,
    // despite the spec saying that it should be an a-rate parameter. We treat
    // it as k-rate. Spec bug: https://www.w3.org/Bugs/Public/show_bug.cgi?id=21592
    UpdateSampleRateIfNeeded(channels);

    uint32_t written = 0;
    TrackTicks streamPosition = aStream->GetCurrentPosition();
    while (written < WEBAUDIO_BLOCK_SIZE) {
      if (mStop != TRACK_TICKS_MAX &&
          streamPosition >= mStop) {
        FillWithZeroes(aOutput, channels, &written, &streamPosition, TRACK_TICKS_MAX);
        continue;
      }
      if (streamPosition < mBeginProcessing) {
        FillWithZeroes(aOutput, channels, &written, &streamPosition,
                       mBeginProcessing);
        continue;
      }
      if (mLoop) {
        // mLoopEnd can become less than mBufferPosition when a LOOPEND engine
        // parameter is received after "loopend" is changed on the node or a
        // new buffer with lower samplerate is set.
        if (mBufferPosition >= mLoopEnd) {
          mBufferPosition = mLoopStart;
        }
        CopyFromBuffer(aStream, aOutput, channels, &written, &streamPosition, mLoopEnd);
      } else {
        if (mBufferPosition < mBufferEnd || mRemainingResamplerTail) {
          CopyFromBuffer(aStream, aOutput, channels, &written, &streamPosition, mBufferEnd);
        } else {
          FillWithZeroes(aOutput, channels, &written, &streamPosition, TRACK_TICKS_MAX);
        }
      }
    }

    // We've finished if we've gone past mStop, or if we're past mDuration when
    // looping is disabled.
    if (streamPosition >= mStop ||
        (!mLoop && mBufferPosition >= mBufferEnd && !mRemainingResamplerTail)) {
      *aFinished = true;
    }
  }

  double mStart; // including the fractional position between ticks
  // Low pass filter effects from the resampler mean that samples before the
  // start time are influenced by resampling the buffer.  mBeginProcessing
  // includes the extent of this filter.  The special value of -TRACK_TICKS_MAX
  // indicates that the resampler has begun processing.
  TrackTicks mBeginProcessing;
  TrackTicks mStop;
  nsRefPtr<ThreadSharedFloatArrayBufferList> mBuffer;
  SpeexResamplerState* mResampler;
  // mRemainingResamplerTail, like mBufferPosition, and
  // mBufferEnd, is measured in input buffer samples.
  int mRemainingResamplerTail;
  int32_t mBufferEnd;
  int32_t mLoopStart;
  int32_t mLoopEnd;
  int32_t mBufferSampleRate;
  int32_t mBufferPosition;
  uint32_t mChannels;
  float mDopplerShift;
  AudioNodeStream* mDestination;
  AudioNodeStream* mSource;
  AudioParamTimeline mPlaybackRateTimeline;
  bool mLoop;
};

AudioBufferSourceNode::AudioBufferSourceNode(AudioContext* aContext)
  : AudioNode(aContext,
              2,
              ChannelCountMode::Max,
              ChannelInterpretation::Speakers)
  , mLoopStart(0.0)
  , mLoopEnd(0.0)
  // mOffset and mDuration are initialized in Start().
  , mPlaybackRate(new AudioParam(MOZ_THIS_IN_INITIALIZER_LIST(),
                  SendPlaybackRateToStream, 1.0f))
  , mLoop(false)
  , mStartCalled(false)
  , mStopped(false)
{
  AudioBufferSourceNodeEngine* engine = new AudioBufferSourceNodeEngine(this, aContext->Destination());
  mStream = aContext->Graph()->CreateAudioNodeStream(engine, MediaStreamGraph::SOURCE_STREAM);
  engine->SetSourceStream(static_cast<AudioNodeStream*>(mStream.get()));
  mStream->AddMainThreadListener(this);
}

AudioBufferSourceNode::~AudioBufferSourceNode()
{
  if (Context()) {
    Context()->UnregisterAudioBufferSourceNode(this);
  }
}

JSObject*
AudioBufferSourceNode::WrapObject(JSContext* aCx, JS::Handle<JSObject*> aScope)
{
  return AudioBufferSourceNodeBinding::Wrap(aCx, this);
}

void
AudioBufferSourceNode::Start(double aWhen, double aOffset,
                             const Optional<double>& aDuration, ErrorResult& aRv)
{
  if (!WebAudioUtils::IsTimeValid(aWhen) ||
      (aDuration.WasPassed() && !WebAudioUtils::IsTimeValid(aDuration.Value()))) {
    aRv.Throw(NS_ERROR_DOM_NOT_SUPPORTED_ERR);
    return;
  }

  if (mStartCalled) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return;
  }
  mStartCalled = true;

  AudioNodeStream* ns = static_cast<AudioNodeStream*>(mStream.get());
  if (!ns) {
    // Nothing to play, or we're already dead for some reason
    return;
  }

  // Remember our arguments so that we can use them when we get a new buffer.
  mOffset = aOffset;
  mDuration = aDuration.WasPassed() ? aDuration.Value()
                                    : std::numeric_limits<double>::min();
  // We can't send these parameters without a buffer because we don't know the
  // buffer's sample rate or length.
  if (mBuffer) {
    SendOffsetAndDurationParametersToStream(ns);
  }

  // Don't set parameter unnecessarily
  if (aWhen > 0.0) {
    ns->SetDoubleParameter(START, mContext->DOMTimeToStreamTime(aWhen));
  }
}

void
AudioBufferSourceNode::SendBufferParameterToStream(JSContext* aCx)
{
  AudioNodeStream* ns = static_cast<AudioNodeStream*>(mStream.get());
  MOZ_ASSERT(ns, "Why don't we have a stream here?");

  if (mBuffer) {
    float rate = mBuffer->SampleRate();
    nsRefPtr<ThreadSharedFloatArrayBufferList> data =
      mBuffer->GetThreadSharedChannelsForRate(aCx);
    ns->SetBuffer(data.forget());
    ns->SetInt32Parameter(SAMPLE_RATE, rate);

    if (mStartCalled) {
      SendOffsetAndDurationParametersToStream(ns);
    }
  } else {
    ns->SetBuffer(nullptr);

    MarkInactive();
  }
}

void
AudioBufferSourceNode::SendOffsetAndDurationParametersToStream(AudioNodeStream* aStream)
{
  NS_ASSERTION(mBuffer && mStartCalled,
               "Only call this when we have a buffer and start() has been called");

  float rate = mBuffer->SampleRate();
  int32_t bufferEnd = mBuffer->Length();
  int32_t offsetSamples = std::max(0, NS_lround(mOffset * rate));

  // Don't set parameter unnecessarily
  if (offsetSamples > 0) {
    aStream->SetInt32Parameter(BUFFERSTART, offsetSamples);
  }

  if (mDuration != std::numeric_limits<double>::min()) {
    bufferEnd = std::min(bufferEnd,
                         offsetSamples + NS_lround(mDuration * rate));
  }
  aStream->SetInt32Parameter(BUFFEREND, bufferEnd);

  MarkActive();
}

void
AudioBufferSourceNode::Stop(double aWhen, ErrorResult& aRv)
{
  if (!WebAudioUtils::IsTimeValid(aWhen)) {
    aRv.Throw(NS_ERROR_DOM_NOT_SUPPORTED_ERR);
    return;
  }

  if (!mStartCalled) {
    aRv.Throw(NS_ERROR_DOM_INVALID_STATE_ERR);
    return;
  }

  AudioNodeStream* ns = static_cast<AudioNodeStream*>(mStream.get());
  if (!ns || !Context()) {
    // We've already stopped and had our stream shut down
    return;
  }

  ns->SetStreamTimeParameter(STOP, Context(), std::max(0.0, aWhen));
}

void
AudioBufferSourceNode::NotifyMainThreadStateChanged()
{
  if (mStream->IsFinished()) {
    class EndedEventDispatcher : public nsRunnable
    {
    public:
      explicit EndedEventDispatcher(AudioBufferSourceNode* aNode)
        : mNode(aNode) {}
      NS_IMETHODIMP Run()
      {
        // If it's not safe to run scripts right now, schedule this to run later
        if (!nsContentUtils::IsSafeToRunScript()) {
          nsContentUtils::AddScriptRunner(this);
          return NS_OK;
        }

        mNode->DispatchTrustedEvent(NS_LITERAL_STRING("ended"));
        return NS_OK;
      }
    private:
      nsRefPtr<AudioBufferSourceNode> mNode;
    };
    if (!mStopped) {
      // Only dispatch the ended event once
      NS_DispatchToMainThread(new EndedEventDispatcher(this));
      mStopped = true;
    }

    // Drop the playing reference
    // Warning: The below line might delete this.
    MarkInactive();
  }
}

void
AudioBufferSourceNode::SendPlaybackRateToStream(AudioNode* aNode)
{
  AudioBufferSourceNode* This = static_cast<AudioBufferSourceNode*>(aNode);
  SendTimelineParameterToStream(This, PLAYBACKRATE, *This->mPlaybackRate);
}

void
AudioBufferSourceNode::SendDopplerShiftToStream(double aDopplerShift)
{
  SendDoubleParameterToStream(DOPPLERSHIFT, aDopplerShift);
}

void
AudioBufferSourceNode::SendLoopParametersToStream()
{
  // Don't compute and set the loop parameters unnecessarily
  if (mLoop && mBuffer) {
    float rate = mBuffer->SampleRate();
    double length = (double(mBuffer->Length()) / mBuffer->SampleRate());
    double actualLoopStart, actualLoopEnd;
    if (mLoopStart >= 0.0 && mLoopEnd > 0.0 &&
        mLoopStart < mLoopEnd) {
      MOZ_ASSERT(mLoopStart != 0.0 || mLoopEnd != 0.0);
      actualLoopStart = (mLoopStart > length) ? 0.0 : mLoopStart;
      actualLoopEnd = std::min(mLoopEnd, length);
    } else {
      actualLoopStart = 0.0;
      actualLoopEnd = length;
    }
    int32_t loopStartTicks = NS_lround(actualLoopStart * rate);
    int32_t loopEndTicks = NS_lround(actualLoopEnd * rate);
    if (loopStartTicks < loopEndTicks) {
      SendInt32ParameterToStream(LOOPSTART, loopStartTicks);
      SendInt32ParameterToStream(LOOPEND, loopEndTicks);
      SendInt32ParameterToStream(LOOP, 1);
    } else {
      // Be explicit about looping not happening if the offsets make
      // looping impossible.
      SendInt32ParameterToStream(LOOP, 0);
    }
  } else if (!mLoop) {
    SendInt32ParameterToStream(LOOP, 0);
  }
}

}
}
