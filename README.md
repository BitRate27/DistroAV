_**NOTE:** `OBS-NDI` was renamed to `DistroAV` ~2024/06 per [obsproject.com](https://obsproject.com)'s request to drop `OBS` from the plugin name._

DistroAV _(Formerly OBS-NDI)_
==============
Network Audio/Video in OBS-Studio using NDI® technology

## Status

`master`: [![master: Push](https://github.com/obs-ndi/obs-ndi/actions/workflows/push.yaml/badge.svg)](https://github.com/obs-ndi/obs-ndi/actions/workflows/push.yaml)
`develop`: [![develop: Pull Request](https://github.com/obs-ndi/obs-ndi/actions/workflows/pr-pull.yaml/badge.svg?branch=develop)](https://github.com/obs-ndi/obs-ndi/actions/workflows/pr-pull.yaml)


## Project's Community

Consider supporting the project with your [Donations](https://opencollective.com/obs-ndi)

[![Discord Shield](https://discordapp.com/api/guilds/1082173788101279746/widget.png?style=banner2)](https://discord.gg/ZuTxbUK3ug)  
(English Speaking)

- DistroAV is an independent open-source project, maintained exclusively by volunteers.


## Plugin features

- **NDI Source** : Receive an NDI stream in OBS
- **NDI Output** : Transmit an NDI Stream of OBS audio and video (Program / Preview)
- **NDI Filter** (a.k.a NDI Dedicated Output) : Transmit an NDI stream of a single OBS source or scene


# Installation

See [Installation Wiki](https://github.com/obs-ndi/obs-ndi/wiki/1.-Installation)

## Requirements

* OBS - [Open Braodcaster Software](https://obsproject.com/download) >= 30.0.0 
* NDI® 6 Runtime - [Installed separately](#installation)


## NDI® Format Supported

DistroAV supports NDI® High Bandwidth (encode/send, decode/receive) and NDI® HX2 (decode/receive).
[Read more more about NDI® Formats on NDI.video](https://ndi.video/tech/formats/)

| Protocol | Decode (Receive) | Encode (Send) |
| --------- | ------------------- | ---------------- |
| NDI® High Bandwidth | Yes | Yes |
| NDI® HX2 | Yes | No |
| NDI® HX3 | Yes | No |

Note: The NDI protocol require substantial bandwidth (~130Mbps for 1080P60 with NDI High Bandwidth), making it suited for fast local network and is not aimed to replace streaming protocol like RTMP/SRT/HLS/WebRTC.


## NDI® Features Supported

This plugin is a community effort to support the NDI® Protocol, not all the features offered by NDI® are supported or implemented yet.

| Feature | Supported | Since version |
| --------- | ------------------- | ---------------- |
| NDI High Bandwidth Decoding | Yes | 0 |
| NDI High Bandwidth Encoding | Yes | 0 |
| NDI HX2 Decoding | Yes |  |
| NDI HX2 Encoding | No |  |
| NDI HX3 Decoding | Yes |  |
| NDI HX3 Encoding | No |  |
| HDR Decoding | tbc |  |
| HDR Encoding | No |  |
| Tally Status | Yes | <4.0 |
| Alpha Channel | Yes | <4.0 |
| PTZ control | TBR | 6.0.0 (planned) |
| NDI Bridge Utility Embedded | No |  |


# Troubleshooting

See [Troubleshooting Wiki](https://github.com/obs-ndi/obs-ndi/wiki/2.-Troubleshooting)


# Development

See [Development Wiki](https://github.com/obs-ndi/obs-ndi/wiki/3.-Development)


## What is NDI® ?

> NDI stands for Network Device Interface, a video connectivity standard that enables multimedia systems to identify and communicate with one another over IP and to encode, transmit, and receive high-quality, low latency, frame-accurate video and audio, and exchange metadata in real-time.

>NDI operates bi-directionally, with many streams on a shared connection. Its encoding algorithm is resolution and frame-rate-independent, supporting 4K resolutions and beyond, along with unlimited floating-point audio channels and custom metadata.

[Read more at NDI.video](https://ndi.video/tech/)


### Use-case example for NDI® protocol

- Receive an NDI stream from a PTZ Camera
- Receive an NDI stream from an encoder
- Receive multiple audio stream with the video from an encoder
- Send an NDI stream to a decoder
- Send multiple audio stream with the video from an encoder
- Send the tally status to a camera operator
- Receive the tally status of an AV Source