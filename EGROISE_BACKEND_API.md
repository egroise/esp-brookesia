# ia-backend-1 API

Fastify web service: STT (OpenAI Whisper) + TTS (Gemini), with distribution of
update and firmware files for devices.

- Default URL: `http://localhost:4000` (port can be overridden via `PORT`)
- No authentication.
- Errors are returned as `{ "error": "message" }`, unless stated otherwise.
- OpenAPI specification: [openapi.yaml](openapi.yaml)

## Contents

| Method | Route                  | Description                                          |
| ------ | ---------------------- | ---------------------------------------------------- |
| GET    | `/health`              | Checks that the service is responding                |
| POST   | `/in`                  | Transcribes audio, starts synthesis asynchronously   |
| GET    | `/out/{callId}`        | Retrieves the synthesized audio of a call            |
| GET    | `/update`              | Lists update files                                   |
| POST   | `/update`              | Downloads an update file                             |
| GET    | `/firmware`            | Information about the current firmware               |
| GET    | `/firmware/{version}`  | Downloads the binary of a firmware version           |

---

## Voice

### Asynchronous flow

1. `POST /in` with a wav file → immediate transcription, returns a `callId`
2. Speech synthesis of the text is started in the background.
3. Poll `GET /out/{callId}` until the audio is available.

Calls are stored in memory: they are lost when the server restarts.

### `POST /in`

Transcribes the audio file with Whisper, then starts Gemini synthesis of the
text without waiting for it to finish.

**Request**: `multipart/form-data` with an audio file (wav). The field name is
free, only the first file is read. Max size: 25 MB.

```bash
curl -F "file=@question.wav" http://localhost:4000/in
```

**Responses**

| Code | Body                                    | Case                              |
| ---- | --------------------------------------- | --------------------------------- |
| 200  | `{ "callId": "<uuid>", "text": "..." }` | Transcription succeeded           |
| 400  | `{ "error": "..." }`                    | No file in the request            |
| 413  | —                                       | File larger than 25 MB            |
| 502  | `{ "error": "..." }`                    | Whisper transcription failed      |

### `GET /out/{callId}`

Retrieves the result of the speech synthesis started by `POST /in`.

```bash
curl -o response.wav http://localhost:4000/out/<callId>
```

**Responses**

| Code | Content-Type       | Body                                                     | Case                   |
| ---- | ------------------ | -------------------------------------------------------- | ---------------------- |
| 200  | `application/json` | `{ "status": "processing", "callId": "<uuid>" }`         | Synthesis in progress  |
| 200  | `audio/wav`        | Audio (`Content-Disposition: inline; filename="<callId>.wav"`) | Synthesis complete |
| 404  | `application/json` | `{ "error": "unknown callId." }`                         | Unknown callId         |
| 500  | `application/json` | `{ "status": "error", "callId": "<uuid>", "error": "..." }` | Synthesis failed    |

To tell the two 200 cases apart, check the response `Content-Type`.


## Device

### `GET /update`

Recursively lists the files in the `device/update` folder with their md5.
Returns an empty list if the folder does not exist.

```json
{
  "files": [
    { "name": "sounds/boot.wav", "md5": "d41d8cd98f00b204e9800998ecf8427e" }
  ]
}
```

`name` is the path relative to `device/update`, using `/` as separator.

| Code | Case                                  |
| ---- | ------------------------------------- |
| 200  | File list                             |
| 500  | Error reading the folder or computing the md5 |

### `POST /update`

Downloads an update file.

**Request**: `application/json`

```json
{ "file": "sounds/boot.wav" }
```

`file` is a path relative to `device/update`, as returned by `GET /update`.

```bash
curl -H "Content-Type: application/json" -d '{"file":"sounds/boot.wav"}' \
  -o boot.wav http://localhost:4000/update
```

**Responses**

| Code | Body                                                          | Case |
| ---- | ------------------------------------------------------------- | ---- |
| 200  | File as `application/octet-stream` (`Content-Disposition: attachment`, `Content-Length`) | Success |
| 400  | `{ "error": "..." }` | `file` missing, path outside `device/update`, or not a file |
| 404  | `{ "error": "Fichier introuvable." }` | File not found |
| 500  | `{ "error": "..." }` | File access error |

### `GET /firmware`

Reads the version from `device/firmware/VERSION` and returns the size and md5 of
the matching `device/firmware/firmware_<version>.bin` file.

```json
{ "version": "1.2.0", "size": 1048576, "md5": "d41d8cd98f00b204e9800998ecf8427e" }
```

| Code | Case                                             |
| ---- | ------------------------------------------------ |
| 200  | Firmware information                             |
| 404  | `VERSION` file or firmware file not found        |
| 500  | Read error                                       |

### `GET /firmware/{version}`

Downloads the binary `device/firmware/firmware_<version>.bin`.
`version` must match `^[a-zA-Z0-9._-]+$`.

```bash
curl -o firmware.bin http://localhost:4000/firmware/1.2.0
```

| Code | Body | Case |
| ---- | ---- | ---- |
| 200  | Binary as `application/octet-stream` (`Content-Disposition: attachment`, `Content-Length`) | Success |
| 400  | `{ "error": "..." }` | Invalid version or path is not a file |
| 404  | `{ "error": "Firmware introuvable." }` | Version does not exist |
| 500  | `{ "error": "..." }` | File access error |

---

## System

### `GET /health`

```json
{ "status": "ok" }
```
