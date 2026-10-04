# Quinco Chat Server

## Build and run

Requires a C++20 compiler and GNU Make. From this directory:

```sh
make
./build/quinco-chat-server
```

The server listens on `127.0.0.1:8080` by default and creates a `data/`
directory for accounts, rooms, messages, and uploads. Stop it with Ctrl+C.

Options:

```sh
./build/quinco-chat-server --host 0.0.0.0 --port 9000 --data ./data --web ./public
```

Open `http://127.0.0.1:8080` for the HTML sign-in and account-creation UI.
`GET /health` returns a JSON health response. The WebSocket endpoint is `/ws`.
The server supports registration, login, session resume, direct conversations,
public rooms, persistent message history, and text messaging. Audio and video
calls use browser WebRTC, so the browser needs microphone/camera permission.
Remote access requires HTTPS for media permissions; localhost is allowed over
HTTP by browsers.

Run the foundation tests with:

```sh
make test
```

## Release installers

The GitHub Actions workflow at `.github/workflows/build.yml` builds and tests
native Linux, macOS, and Windows versions. Run it with **Build Installers** in
the Actions tab or push a `v*` tag. Each job publishes its installer as a
downloadable workflow artifact:

- Linux: `QuincoChat.AppImage`
- macOS: `QuincoChat-setup.pkg` (universal arm64 and x86_64 binary)
- Windows: `QuincoChat-setup.exe` (64-bit installer)

The Actions artifact is a ZIP. After extracting it, either restore the raw
AppImage's executable bit with `chmod +x QuincoChat.AppImage`, or extract
`QuincoChat.AppImage.tar.gz` to preserve its permissions. Then run
`./QuincoChat.AppImage`.

The installers include the server executable and HTML client. Calls include a
public STUN server for connection negotiation; networks that block direct
peer-to-peer traffic may require a TURN relay.