*** Settings ***
Documentation       End-to-end tests of codec-encode and codec-decode. They run the real programs on
...                 real files, as a user would. Exit code 2 means wrong usage, 1 means an error.
...
...                 Start from the root of the repository and name the folder of the tools:
...                 robot --variable TOOLS_DIR:build/default --outputdir build/robot tests/e2e

Library             OperatingSystem
Library             Process
Library             AudioLibrary.py

Suite Setup         Prepare The Tools And A Working Directory
Suite Teardown      Remove Directory    ${WORK}    recursive=True


*** Variables ***
${TOOLS_DIR}        ${EMPTY}
${ONE_SECOND}       48000


*** Test Cases ***
Decoded File Has The Format And Length Of The Input
    ${input}=    Create Signal    mixture    ${ONE_SECOND}
    ${decoded}=    Encode And Decode    ${input}    64000
    Wav Should Have Format    ${decoded}    ${ONE_SECOND}

Length Is Kept For Any Signal Length
    [Template]    Length Should Be Kept
    0
    1
    1023
    1024
    1025
    5000

Bitstream Stays Within The Bitrate
    [Template]    Stream Should Fit The Bitrate
    8000
    32000
    64000
    128000
    512000

Quality Grows With The Bitrate
    ${input}=    Create Signal    mixture    ${ONE_SECOND}
    ${low}=    Snr At Bitrate    ${input}    16000
    ${middle}=    Snr At Bitrate    ${input}    64000
    ${high}=    Snr At Bitrate    ${input}    256000
    Should Be True    ${low} < ${middle} < ${high}

A Tone Reaches The Expected Quality
    [Documentation]    The abrupt start and end of the test signal lower this value, and the model
    ...    aims at noise below its thresholds rather than at a high SNR.
    ${input}=    Create Signal    tone    ${ONE_SECOND}
    ${snr}=    Snr At Bitrate    ${input}    64000
    Log    ${snr}
    Should Be True    ${snr} > 20

The Psychoacoustic Model Is On By Default
    ${input}=    Create Signal    mixture    10000
    ${default}=    Encode    ${input}    64000    default.bin
    ${with_model}=    Set Variable    ${WORK}${/}with_model.bin
    Run Successfully    ${ENCODER}    ${input}    ${with_model}    --bitrate    64000    --psy    on
    ${default_bytes}=    Get Binary File    ${default}
    ${with_model_bytes}=    Get Binary File    ${with_model}
    Should Be Equal    ${default_bytes}    ${with_model_bytes}

The Baseline Gives A Higher SNR Than The Model
    [Documentation]    Flat noise is the best choice for the SNR, but not for the ear. The SNR is the
    ...    one thing that must get worse when the model moves noise to where it is masked.
    ${input}=    Create Signal    mixture    ${ONE_SECOND}
    ${with_model}=    Snr At Bitrate    ${input}    64000
    ${stream}=    Set Variable    ${WORK}${/}baseline.bin
    ${decoded}=    Set Variable    ${WORK}${/}baseline.wav
    Run Successfully    ${ENCODER}    ${input}    ${stream}    --bitrate    64000    --psy    off
    Run Successfully    ${DECODER}    ${stream}    ${decoded}
    ${baseline}=    Snr Db    ${input}    ${decoded}
    Should Be True    ${baseline} > ${with_model}

Silence Stays Silent
    ${input}=    Create Signal    silence    10000
    ${decoded}=    Encode And Decode    ${input}    64000
    Wav Should Be Silent    ${decoded}

A Full Scale Signal Works At The Lowest Bitrate
    ${input}=    Create Signal    square    10000
    ${decoded}=    Encode And Decode    ${input}    8000
    Wav Should Have Format    ${decoded}    10000

Encoder Gives The Same File For The Same Input
    ${input}=    Create Signal    mixture    ${ONE_SECOND}
    ${first}=    Encode    ${input}    64000    first.bin
    ${second}=    Encode    ${input}    64000    second.bin
    ${first_bytes}=    Get Binary File    ${first}
    ${second_bytes}=    Get Binary File    ${second}
    Should Be Equal    ${first_bytes}    ${second_bytes}

Paths May Contain Spaces
    Create Directory    ${WORK}${/}a folder with spaces
    ${input}=    Set Variable    ${WORK}${/}a folder with spaces${/}my input.wav
    ${stream}=    Set Variable    ${WORK}${/}a folder with spaces${/}my stream.bin
    ${decoded}=    Set Variable    ${WORK}${/}a folder with spaces${/}my output.wav
    Write Test Signal    ${input}    tone    5000
    Run Successfully    ${ENCODER}    ${input}    ${stream}    --bitrate    64000
    Run Successfully    ${DECODER}    ${stream}    ${decoded}
    Wav Should Have Format    ${decoded}    5000

Encoder Reports Wrong Usage
    ${input}=    Create Signal    tone    1000
    Should Fail With Code    2    ${ENCODER}
    Should Fail With Code    2    ${ENCODER}    ${input}
    Should Fail With Code    2    ${ENCODER}    ${input}    ${WORK}${/}out.bin
    Should Fail With Code    2    ${ENCODER}    ${input}    ${WORK}${/}out.bin    --bitrate
    Should Fail With Code    2    ${ENCODER}    ${input}    ${WORK}${/}out.bin    --bitrate    abc
    Should Fail With Code    2    ${ENCODER}    ${input}    ${WORK}${/}out.bin    --bitrate    -1
    Should Fail With Code    2    ${ENCODER}    ${input}    ${WORK}${/}out.bin    --bitrate    64000    --psy
    Should Fail With Code    2    ${ENCODER}    ${input}    ${WORK}${/}out.bin    --bitrate    64000    --psy    maybe
    File Should Not Exist    ${WORK}${/}out.bin

Encoder Rejects Unsupported Input
    [Template]    Encoder Should Reject
    samples=1000    sample_rate=44100
    samples=1000    channels=2
    samples=1000    sample_width=3

Encoder Rejects A Bitrate Out Of Range
    ${input}=    Create Signal    tone    1000
    Should Fail With Code    1    ${ENCODER}    ${input}    ${WORK}${/}out.bin    --bitrate    7999
    Should Fail With Code    1    ${ENCODER}    ${input}    ${WORK}${/}out.bin    --bitrate    512001
    File Should Not Exist    ${WORK}${/}out.bin

Encoder Rejects Files It Cannot Read
    Should Fail With Code    1    ${ENCODER}    ${WORK}${/}missing.wav    ${WORK}${/}out.bin    --bitrate    64000
    Create File    ${WORK}${/}not_audio.wav    This is not audio.
    Should Fail With Code    1    ${ENCODER}    ${WORK}${/}not_audio.wav    ${WORK}${/}out.bin    --bitrate    64000
    File Should Not Exist    ${WORK}${/}out.bin

Decoder Reports Wrong Usage
    Should Fail With Code    2    ${DECODER}
    Should Fail With Code    2    ${DECODER}    only_one_argument

Decoder Rejects Files It Cannot Read
    ${input}=    Create Signal    tone    1000
    Should Fail With Code    1    ${DECODER}    ${WORK}${/}missing.bin    ${WORK}${/}out.wav
    Should Fail With Code    1    ${DECODER}    ${input}    ${WORK}${/}out.wav
    File Should Not Exist    ${WORK}${/}out.wav

Decoder Rejects Damaged Bitstreams
    ${input}=    Create Signal    tone    5000
    ${stream}=    Encode    ${input}    64000
    ${size}=    Get File Size    ${stream}
    ${damaged}=    Set Variable    ${WORK}${/}damaged.bin
    Copy Truncated    ${stream}    ${damaged}    ${size - 1}
    Should Fail With Code    1    ${DECODER}    ${damaged}    ${WORK}${/}out.wav
    Copy Truncated    ${stream}    ${damaged}    10
    Should Fail With Code    1    ${DECODER}    ${damaged}    ${WORK}${/}out.wav
    Copy With Damaged Byte    ${stream}    ${damaged}    0
    Should Fail With Code    1    ${DECODER}    ${damaged}    ${WORK}${/}out.wav
    File Should Not Exist    ${WORK}${/}out.wav


*** Keywords ***
Prepare The Tools And A Working Directory
    Should Not Be Empty    ${TOOLS_DIR}    Name the folder of the tools with --variable TOOLS_DIR:folder.
    ${encoder}=    Tool Path    ${TOOLS_DIR}    codec-encode
    ${decoder}=    Tool Path    ${TOOLS_DIR}    codec-decode
    Set Suite Variable    ${ENCODER}    ${encoder}
    Set Suite Variable    ${DECODER}    ${decoder}
    Set Suite Variable    ${WORK}    ${TEMPDIR}${/}audio_codec_lab_e2e
    Remove Directory    ${WORK}    recursive=True
    Create Directory    ${WORK}

Create Signal
    [Arguments]    ${kind}    ${samples}
    ${path}=    Set Variable    ${WORK}${/}${kind}_${samples}.wav
    Write Test Signal    ${path}    ${kind}    ${samples}
    RETURN    ${path}

Run Successfully
    [Arguments]    @{command}
    ${result}=    Run Process    @{command}
    Should Be Equal As Integers    ${result.rc}    0    ${result.stderr}
    RETURN    ${result}

Should Fail With Code
    [Arguments]    ${code}    @{command}
    ${result}=    Run Process    @{command}
    Should Be Equal As Integers    ${result.rc}    ${code}    ${result.stdout} ${result.stderr}
    Should Not Be Empty    ${result.stderr}    A failing tool must say why.

Encode
    [Arguments]    ${input}    ${bitrate}    ${name}=stream.bin
    ${stream}=    Set Variable    ${WORK}${/}${name}
    Run Successfully    ${ENCODER}    ${input}    ${stream}    --bitrate    ${bitrate}
    RETURN    ${stream}

Encode And Decode
    [Arguments]    ${input}    ${bitrate}
    ${stream}=    Encode    ${input}    ${bitrate}
    ${decoded}=    Set Variable    ${WORK}${/}decoded.wav
    Run Successfully    ${DECODER}    ${stream}    ${decoded}
    RETURN    ${decoded}

Snr At Bitrate
    [Arguments]    ${input}    ${bitrate}
    ${decoded}=    Encode And Decode    ${input}    ${bitrate}
    ${snr}=    Snr Db    ${input}    ${decoded}
    RETURN    ${snr}

Length Should Be Kept
    [Arguments]    ${samples}
    ${input}=    Create Signal    mixture    ${samples}
    ${decoded}=    Encode And Decode    ${input}    64000
    Wav Should Have Format    ${decoded}    ${samples}

Stream Should Fit The Bitrate
    [Arguments]    ${bitrate}
    ${input}=    Create Signal    mixture    ${ONE_SECOND}
    ${stream}=    Encode    ${input}    ${bitrate}
    ${size}=    Get File Size    ${stream}
    ${allowed}=    Largest Allowed Stream Size    ${ONE_SECOND}    ${bitrate}
    Should Be True    ${size} <= ${allowed}    ${size} bytes are more than the ${allowed} allowed.

Encoder Should Reject
    [Arguments]    &{signal}
    ${input}=    Set Variable    ${WORK}${/}unsupported.wav
    Write Test Signal    ${input}    tone    &{signal}
    Should Fail With Code    1    ${ENCODER}    ${input}    ${WORK}${/}out.bin    --bitrate    64000
    File Should Not Exist    ${WORK}${/}out.bin
