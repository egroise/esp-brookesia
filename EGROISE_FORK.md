# examples/agent/chatbot

This modified chatbot works with a backend described in API.md.

- No special UI for OTA done yet.

## VERY BASIC Chat API

`POST /in` sends wav file and returns callerid
`GET /out/{callId}` returns wav file (or busy status)

## Firmware OTA update using standard system

`GET /firmware` : gets the current firmware

`GET /firmware/{version}` : gets the requested firmware

`otadata` partition tells the firmware partition to boot

`ota_0` and `ota_1` are te dual-partitions scheme for firmware update.


## Files moved from partition to SD

`anim_icon.bin` is now on sd root folder 

`settings.json` tells where the backend is
```json
{
    "CONFIG_EXAMPLE_AGENTS_CUSTOM_IA_SERVER_URL" : "http://172.20.10.4:4000"
}
```
The `/update` folder contains all the files that are OTA updated.

## OTA Update of SD files in /update

`GET /update` returns the list of current files with their MD5. If a file on SD has different MD5, it is downloaded using `POST /update` route.

