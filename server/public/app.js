(() => {
  const byId = (id) => document.getElementById(id);
  const authView = byId("auth-view");
  const workspace = byId("workspace");
  const authForm = byId("auth-form");
  const authMessage = byId("form-message");
  const authSubmit = byId("submit-button");
  const authSubmitLabel = byId("submit-label");
  const usernameInput = byId("username");
  const passwordInput = byId("password");
  const displayNameInput = byId("display-name");
  const tokenKey = "quinco.session";
  const roomMap = new Map();
  const userMap = new Map();
  const onlineUsers = new Set();
  const messagesByRoom = new Map();
  let socket = null;
  let authMode = "login";
  let authPending = false;
  let currentUser = null;
  let activeRoomId = "";
  let actionMode = "";
  let callState = null;
  let reconnectTimer = 0;
  let reconnectDelay = 500;
  let toastTimer = 0;
  let typingTimer = 0;
  let lastTypingSent = 0;

  function setConnection(state, text) {
    byId("connection").dataset.state = state;
    byId("connection-text").textContent = text;
    byId("workspace-connection").textContent = state === "connected"
      ? "Connected"
      : "Reconnecting";
    byId("workspace-connection").dataset.state = state;
  }

  function setAuthMode(mode) {
    authMode = mode;
    const registering = mode === "register";
    byId("login-tab").setAttribute("aria-selected", String(!registering));
    byId("register-tab").setAttribute("aria-selected", String(registering));
    byId("display-name-field").hidden = !registering;
    displayNameInput.required = registering;
    passwordInput.autocomplete = registering ? "new-password" : "current-password";
    byId("auth-title").textContent = registering ? "Make yourself at home" : "Welcome back";
    byId("auth-subtitle").textContent = registering
      ? "Create an account to join your people."
      : "Sign in to pick up where your conversations left off.";
    authSubmitLabel.textContent = registering ? "Create account" : "Sign in";
    authMessage.textContent = "";
    authMessage.classList.remove("success");
  }

  function finishAuthRequest() {
    authPending = false;
    authSubmit.disabled = false;
    authSubmitLabel.textContent = authMode === "register" ? "Create account" : "Sign in";
  }

  function sendPacket(packet) {
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      showToast("The server connection is offline. Reconnecting now.");
      return false;
    }
    socket.send(JSON.stringify(packet));
    return true;
  }

  function showToast(text) {
    const toast = byId("toast");
    toast.textContent = text;
    toast.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { toast.hidden = true; }, 4000);
  }

  function applyDirectory(directory) {
    for (const user of directory.users || []) userMap.set(user.id, user);
    onlineUsers.clear();
    for (const userId of directory.online || []) onlineUsers.add(userId);
    renderRooms();
    renderBrowseRooms(directory.rooms || []);
  }

  function roomLabel(room) {
    if (!room.direct) return room.name || "Untitled room";
    const peerId = (room.members || []).find((memberId) => memberId !== currentUser?.id);
    const peer = userMap.get(peerId);
    return peer?.displayName || peer?.username || "Direct conversation";
  }

  function renderRooms() {
    const list = byId("room-list");
    list.replaceChildren();
    const rooms = Array.from(roomMap.values()).sort((first, second) => {
      return (second.lastActivity || second.createdAt || 0) -
        (first.lastActivity || first.createdAt || 0);
    });
    byId("room-empty").hidden = rooms.length > 0;

    for (const room of rooms) {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "room-row";
      button.dataset.roomId = room.id;
      button.setAttribute("aria-current", String(room.id === activeRoomId));
      const glyph = document.createElement("span");
      glyph.className = "room-row-glyph";
      glyph.setAttribute("aria-hidden", "true");
      glyph.textContent = room.direct ? "◉" : "#";
      const name = document.createElement("span");
      name.className = "room-row-name";
      name.textContent = roomLabel(room);
      button.setAttribute("aria-label", name.textContent);
      button.append(glyph, name);
      button.addEventListener("click", () => selectRoom(room.id));
      list.append(button);
    }
  }

  function showWorkspace(user) {
    currentUser = user;
    authView.hidden = true;
    workspace.hidden = false;
    const displayName = user.displayName || user.username || "friend";
    byId("welcome-name").textContent = displayName;
    byId("account-name").textContent = displayName;
    byId("account-handle").textContent = "@" + (user.username || "");
    byId("account-avatar").textContent = Array.from(displayName.trim())[0]?.toUpperCase() || "Q";
    renderRooms();
  }

  function showAuth() {
    endCall(false);
    currentUser = null;
    activeRoomId = "";
    roomMap.clear();
    userMap.clear();
    messagesByRoom.clear();
    workspace.hidden = true;
    authView.hidden = false;
    authForm.reset();
    setAuthMode("login");
  }

  function selectRoom(roomId) {
    const room = roomMap.get(roomId);
    if (!room) return;
    activeRoomId = roomId;
    byId("workspace-title").textContent = roomLabel(room);
    byId("welcome-state").hidden = true;
    byId("chat-view").hidden = false;
    byId("message-input").disabled = false;
    byId("send-button").disabled = false;
    byId("audio-call-button").hidden = !room.direct;
    byId("video-call-button").hidden = !room.direct;
    byId("typing-line").textContent = "";
    if (!messagesByRoom.has(roomId)) messagesByRoom.set(roomId, []);
    renderMessages(roomId);
    renderRooms();
    sendPacket({ t: "history", roomId, limit: 60 });
    byId("message-input").focus();
  }

  function renderMessages(roomId) {
    const list = byId("message-list");
    list.replaceChildren();
    const items = messagesByRoom.get(roomId) || [];
    if (!items.length) {
      const empty = document.createElement("div");
      empty.className = "message-empty";
      empty.textContent = "No messages here yet. Start the conversation.";
      list.append(empty);
      return;
    }

    for (const item of items) {
      const user = userMap.get(item.senderId);
      const name = user?.displayName || user?.username ||
        (item.senderId === currentUser?.id ? currentUser.displayName : "Member");
      const row = document.createElement("article");
      row.className = "message-item";
      const avatar = document.createElement("span");
      avatar.className = "message-avatar";
      avatar.textContent = Array.from(name.trim())[0]?.toUpperCase() || "Q";
      const content = document.createElement("div");
      content.className = "message-content";
      const meta = document.createElement("div");
      meta.className = "message-meta";
      const author = document.createElement("span");
      author.className = "message-author";
      author.textContent = name;
      const time = document.createElement("time");
      time.className = "message-time";
      if (item.sentAt) {
        const date = new Date(item.sentAt);
        time.dateTime = date.toISOString();
        time.textContent = date.toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
      }
      meta.append(author, time);
      content.append(meta);
      if (item.body) {
        const body = document.createElement("p");
        body.className = "message-body";
        body.textContent = item.body;
        content.append(body);
      }
      if (item.attachment?.url) {
        const attachment = document.createElement("a");
        attachment.href = item.attachment.url;
        attachment.textContent = item.attachment.name || "Attachment";
        attachment.target = "_blank";
        attachment.rel = "noopener noreferrer";
        content.append(attachment);
      }
      row.append(avatar, content);
      list.append(row);
    }
    list.scrollTop = list.scrollHeight;
  }

  function openActionDialog(mode) {
    actionMode = mode;
    byId("action-error").textContent = "";
    const config = mode === "direct"
      ? ["New conversation", "Enter their username to start a private chat.", "Username", "Open conversation"]
      : ["Create a room", "Give your group a name. You can invite people later.", "Room name", "Create room"];
    byId("action-title").textContent = config[0];
    byId("action-copy").textContent = config[1];
    byId("action-label").textContent = config[2];
    byId("action-submit").textContent = config[3];
    byId("action-input").value = "";
    byId("action-input").maxLength = mode === "direct" ? 24 : 48;
    byId("action-input").pattern = mode === "direct" ? "[A-Za-z0-9._\\x2d]{3,24}" : ".{1,48}";
    byId("action-dialog").showModal();
    byId("action-input").focus();
  }

  function renderBrowseRooms(rooms) {
    const list = byId("discovery-list");
    list.replaceChildren();
    byId("browse-empty").hidden = rooms.length > 0;
    for (const room of rooms) {
      const row = document.createElement("div");
      row.className = "discovery-row";
      const text = document.createElement("div");
      const name = document.createElement("div");
      name.className = "discovery-name";
      name.textContent = room.name || "Untitled room";
      const meta = document.createElement("div");
      meta.className = "discovery-meta";
      meta.textContent = `${room.memberCount || room.members?.length || 0} members`;
      text.append(name, meta);
      const join = document.createElement("button");
      join.type = "button";
      join.textContent = "Join";
      join.addEventListener("click", () => {
        byId("browse-error").textContent = "";
        sendPacket({ t: "room_join", roomId: room.id });
      });
      row.append(text, join);
      list.append(row);
    }
  }

  function openBrowseDialog() {
    byId("browse-error").textContent = "";
    byId("discovery-list").replaceChildren();
    byId("browse-empty").hidden = true;
    byId("browse-dialog").showModal();
    sendPacket({ t: "directory", query: "" });
  }

  function addMessage(item) {
    if (!item?.roomId) return;
    const items = messagesByRoom.get(item.roomId) || [];
    if (!items.some((existing) => existing.id === item.id)) {
      items.push(item);
      items.sort((first, second) => first.sentAt - second.sentAt);
      messagesByRoom.set(item.roomId, items);
    }
    if (activeRoomId === item.roomId) renderMessages(item.roomId);
  }

  function applyRooms(rooms) {
    roomMap.clear();
    for (const room of rooms || []) roomMap.set(room.id, room);
    renderRooms();
    if (activeRoomId && !roomMap.has(activeRoomId)) {
      activeRoomId = "";
      byId("chat-view").hidden = true;
      byId("welcome-state").hidden = false;
      byId("workspace-title").textContent = "Messages";
      byId("audio-call-button").hidden = true;
      byId("video-call-button").hidden = true;
    }
  }

  function showCallDialog(title, status) {
    byId("call-title").textContent = title;
    byId("call-status").textContent = status;
    if (!byId("call-dialog").open) byId("call-dialog").showModal();
  }

  function resetCallDialog() {
    for (const id of ["accept-call-button", "decline-call-button", "mute-call-button",
                      "hangup-call-button", "close-call-button"]) {
      byId(id).hidden = true;
    }
    byId("call-media").hidden = true;
    byId("remote-video").srcObject = null;
    byId("local-video").srcObject = null;
    byId("remote-audio").srcObject = null;
    if (byId("call-dialog").open) byId("call-dialog").close();
  }

  function endCall(sendHangup = true) {
    const state = callState;
    callState = null;
    if (state?.callId && sendHangup) {
      sendPacket({ t: "call_hangup", callId: state.callId });
    }
    if (state?.peerConnection) state.peerConnection.close();
    for (const track of state?.localStream?.getTracks() || []) track.stop();
    resetCallDialog();
  }

  async function setupPeerConnection(createOffer) {
    const state = callState;
    if (!state || state.peerConnection) return;
    if (!navigator.mediaDevices?.getUserMedia || !window.RTCPeerConnection) {
      byId("call-status").textContent = "This browser does not support audio/video calls.";
      return;
    }
    try {
      const stream = await navigator.mediaDevices.getUserMedia({
        audio: true,
        video: state.video ? { facingMode: "user" } : false
      });
      if (callState !== state) {
        for (const track of stream.getTracks()) track.stop();
        return;
      }
      state.localStream = stream;
      const peer = new RTCPeerConnection({
        iceServers: [{ urls: "stun:stun.l.google.com:19302" }]
      });
      state.peerConnection = peer;
      byId("call-media").hidden = false;
      byId("local-video").hidden = !state.video;
      byId("remote-video").hidden = !state.video;
      byId("local-video").srcObject = stream;
      byId("remote-video").muted = true;
      byId("mute-call-button").hidden = false;
      byId("hangup-call-button").hidden = false;
      byId("mute-call-button").textContent = "Mute";
      for (const track of stream.getTracks()) peer.addTrack(track, stream);

      peer.ontrack = (event) => {
        const remoteStream = event.streams[0];
        if (remoteStream) {
          byId("remote-video").srcObject = remoteStream;
          byId("remote-audio").srcObject = remoteStream;
        }
      };
      peer.onicecandidate = (event) => {
        if (event.candidate && callState === state) {
          sendPacket({
            t: "call_signal",
            callId: state.callId,
            kind: "candidate",
            signal: event.candidate.toJSON()
          });
        }
      };
      peer.onconnectionstatechange = () => {
        if (peer.connectionState === "connected") {
          byId("call-status").textContent = "Connected";
        } else if (peer.connectionState === "failed") {
          byId("call-status").textContent = "Could not connect. Try again on a different network.";
        }
      };

      if (createOffer) {
        const offer = await peer.createOffer();
        await peer.setLocalDescription(offer);
        sendPacket({ t: "call_signal", callId: state.callId, kind: "offer", signal: peer.localDescription.toJSON() });
      }
      byId("call-status").textContent = "Connecting…";
      const waiting = state.pendingSignals.splice(0);
      for (const packet of waiting) await processCallSignal(packet);
    } catch (error) {
      byId("call-status").textContent = error.name === "NotAllowedError"
        ? "Allow microphone or camera access to start the call."
        : "Could not start audio/video. Check your device and try again.";
      byId("close-call-button").hidden = false;
      if (state.callId) sendPacket({ t: "call_hangup", callId: state.callId });
    }
  }

  async function processCallSignal(packet) {
    const state = callState;
    const peer = state?.peerConnection;
    if (!state || !peer || packet.callId !== state.callId) {
      if (state) state.pendingSignals.push(packet);
      return;
    }
    try {
      if (packet.kind === "offer") {
        await peer.setRemoteDescription(new RTCSessionDescription(packet.signal));
        const answer = await peer.createAnswer();
        await peer.setLocalDescription(answer);
        sendPacket({ t: "call_signal", callId: state.callId, kind: "answer", signal: peer.localDescription.toJSON() });
        const waiting = state.pendingSignals.splice(0);
        for (const queued of waiting) await processCallSignal(queued);
      } else if (packet.kind === "answer") {
        await peer.setRemoteDescription(new RTCSessionDescription(packet.signal));
        const waiting = state.pendingSignals.splice(0);
        for (const queued of waiting) await processCallSignal(queued);
      } else if (packet.kind === "candidate") {
        if (!peer.remoteDescription) {
          state.pendingSignals.push(packet);
        } else {
          await peer.addIceCandidate(new RTCIceCandidate(packet.signal));
        }
      }
    } catch {
      byId("call-status").textContent = "Call setup failed. End the call and try again.";
    }
  }

  function handleCallMessage(packet) {
    if (packet.t === "call_incoming") {
      if (callState) {
        sendPacket({ t: "call_decline", callId: packet.callId });
        return;
      }
      const caller = packet.caller || {};
      callState = {
        callId: packet.callId,
        roomId: packet.roomId,
        peerId: caller.id,
        role: "callee",
        video: Boolean(packet.video),
        pendingSignals: [],
        localStream: null,
        peerConnection: null
      };
      showCallDialog(packet.video ? "Incoming video call" : "Incoming audio call",
        `${caller.displayName || caller.username || "Someone"} is calling.`);
      byId("accept-call-button").hidden = false;
      byId("decline-call-button").hidden = false;
    } else if (packet.t === "call_ringing") {
      if (!callState || callState.role !== "caller") {
        sendPacket({ t: "call_hangup", callId: packet.callId });
        return;
      }
      callState.callId = packet.callId;
      showCallDialog(packet.video ? "Video call" : "Audio call", "Calling…");
      byId("hangup-call-button").hidden = false;
    } else if (packet.t === "call_accepted") {
      if (!callState) return;
      callState.callId = packet.callId;
      byId("accept-call-button").hidden = true;
      byId("decline-call-button").hidden = true;
      byId("hangup-call-button").hidden = false;
      showCallDialog(callState.video ? "Video call" : "Audio call", "Connecting…");
      setupPeerConnection(callState.role === "caller");
    } else if (packet.t === "call_signal") {
      if (!callState) return;
      processCallSignal(packet);
    } else if (packet.t === "call_ended") {
      const reason = packet.reason;
      endCall(false);
      if (reason === "declined") showToast("The call was declined.");
      else if (reason === "peer_left") showToast("The other person left the call.");
    }
  }

  function handleMessage(event) {
    let packet;
    try {
      packet = JSON.parse(event.data);
    } catch {
      showToast("The server sent an unreadable response.");
      return;
    }

    if (packet.t === "hello") {
      reconnectDelay = 500;
      setConnection("connected", "Connected to your server");
      const token = localStorage.getItem(tokenKey);
      if (token) socket.send(JSON.stringify({ t: "resume", token }));
    } else if (packet.t === "auth_ok") {
      localStorage.setItem(tokenKey, packet.token);
      finishAuthRequest();
      applyDirectory(packet.directory || {});
      applyRooms(packet.rooms || []);
      showWorkspace(packet.user || {});
    } else if (packet.t === "auth_error") {
      if (localStorage.getItem(tokenKey)) localStorage.removeItem(tokenKey);
      finishAuthRequest();
      authMessage.textContent = packet.message || "Could not sign in.";
      authMessage.classList.remove("success");
    } else if (packet.t === "logged_out") {
      localStorage.removeItem(tokenKey);
      finishAuthRequest();
      showAuth();
      if (socket) socket.close();
    } else if (packet.t === "rooms") {
      applyRooms(packet.rooms);
    } else if (packet.t === "directory") {
      applyDirectory(packet);
    } else if (packet.t === "presence") {
      onlineUsers.clear();
      for (const userId of packet.online || []) onlineUsers.add(userId);
      sendPacket({ t: "directory", query: "" });
    } else if (packet.t === "bootstrap") {
      applyDirectory(packet.directory || {});
      applyRooms(packet.rooms || []);
      showWorkspace(packet.user || currentUser || {});
    } else if (packet.t === "room_created" || packet.t === "room_opened" || packet.t === "room_joined") {
      if (packet.room) {
        roomMap.set(packet.room.id, packet.room);
        renderRooms();
        if (packet.t !== "room_joined" || byId("browse-dialog").open) {
          byId("browse-dialog").close();
          selectRoom(packet.room.id);
        }
      }
    } else if (packet.t === "room_left") {
      if (activeRoomId === packet.roomId) {
        activeRoomId = "";
        byId("chat-view").hidden = true;
        byId("welcome-state").hidden = false;
        byId("workspace-title").textContent = "Messages";
        byId("audio-call-button").hidden = true;
        byId("video-call-button").hidden = true;
      }
    } else if (packet.t === "history") {
      messagesByRoom.set(packet.roomId, packet.messages || []);
      if (activeRoomId === packet.roomId) renderMessages(packet.roomId);
    } else if (packet.t === "message_new") {
      addMessage(packet.message);
    } else if (packet.t === "typing") {
      if (packet.roomId === activeRoomId) {
        const user = userMap.get(packet.userId);
        byId("typing-line").textContent = packet.active
          ? `${user?.displayName || user?.username || "Someone"} is typing…`
          : "";
      }
    } else if (packet.t === "profile_updated") {
      if (packet.user) userMap.set(packet.user.id, packet.user);
      renderRooms();
    } else if (packet.t === "error") {
      if (byId("browse-dialog").open) byId("browse-error").textContent = packet.message || "Something went wrong.";
      else if (authView.hidden) showToast(packet.message || "Something went wrong.");
      else authMessage.textContent = packet.message || "Something went wrong.";
    } else if (packet.t.startsWith("call_")) {
      handleCallMessage(packet);
    }
  }

  function connect() {
    clearTimeout(reconnectTimer);
    const scheme = location.protocol === "https:" ? "wss:" : "ws:";
    socket = new WebSocket(`${scheme}//${location.host}/ws`);
    socket.addEventListener("open", () => setConnection("connecting", "Connected; waiting for server"));
    socket.addEventListener("message", handleMessage);
    socket.addEventListener("error", () => setConnection("error", "Server connection failed"));
    socket.addEventListener("close", () => {
      setConnection("error", workspace.hidden ? "Could not reach the server" : "Connection lost");
      finishAuthRequest();
      if (callState) endCall(false);
      reconnectTimer = setTimeout(connect, reconnectDelay);
      reconnectDelay = Math.min(reconnectDelay * 2, 8000);
    });
  }

  authForm.addEventListener("submit", (event) => {
    event.preventDefault();
    if (!authForm.reportValidity()) return;
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      authMessage.textContent = "The server connection is not ready. Try again in a moment.";
      return;
    }
    const packet = {
      t: authMode,
      username: usernameInput.value.trim(),
      password: passwordInput.value
    };
    if (authMode === "register") packet.displayName = displayNameInput.value.trim();
    socket.send(JSON.stringify(packet));
    authPending = true;
    authSubmit.disabled = true;
    authSubmitLabel.textContent = authMode === "register" ? "Creating account…" : "Signing in…";
    authMessage.textContent = "";
  });

  byId("login-tab").addEventListener("click", () => setAuthMode("login"));
  byId("register-tab").addEventListener("click", () => setAuthMode("register"));
  byId("password-toggle").addEventListener("click", (event) => {
    const button = event.currentTarget;
    const revealing = passwordInput.type === "password";
    passwordInput.type = revealing ? "text" : "password";
    button.textContent = revealing ? "Hide" : "Show";
    button.setAttribute("aria-label", revealing ? "Hide password" : "Show password");
    button.setAttribute("aria-pressed", String(revealing));
  });

  byId("new-chat-button").addEventListener("click", () => openActionDialog("direct"));
  byId("create-room-button").addEventListener("click", () => openActionDialog("room"));
  byId("browse-rooms-button").addEventListener("click", openBrowseDialog);
  byId("action-cancel").addEventListener("click", () => byId("action-dialog").close());
  byId("action-form").addEventListener("submit", (event) => {
    event.preventDefault();
    const value = byId("action-input").value.trim();
    if (!value) return;
    sendPacket(actionMode === "direct"
      ? { t: "room_open_direct", username: value }
      : { t: "room_create", name: value });
    byId("action-dialog").close();
  });
  byId("browse-close").addEventListener("click", () => byId("browse-dialog").close());

  byId("composer").addEventListener("submit", (event) => {
    event.preventDefault();
    const input = byId("message-input");
    const body = input.value.trim();
    if (!activeRoomId || !body) return;
    if (sendPacket({ t: "message_send", roomId: activeRoomId, body })) {
      input.value = "";
      sendPacket({ t: "typing", roomId: activeRoomId, active: false });
    }
  });
  byId("message-input").addEventListener("keydown", (event) => {
    if (event.key === "Enter" && !event.shiftKey) {
      event.preventDefault();
      byId("composer").requestSubmit();
    }
  });
  byId("message-input").addEventListener("input", () => {
    const now = Date.now();
    if (now - lastTypingSent > 700 && activeRoomId) {
      sendPacket({ t: "typing", roomId: activeRoomId, active: true });
      lastTypingSent = now;
    }
    clearTimeout(typingTimer);
    typingTimer = setTimeout(() => {
      if (activeRoomId) sendPacket({ t: "typing", roomId: activeRoomId, active: false });
    }, 1400);
  });

  function startCall(video) {
    const room = roomMap.get(activeRoomId);
    if (!room?.direct || callState) return;
    const peerId = (room.members || []).find((memberId) => memberId !== currentUser?.id);
    if (!peerId) return;
    callState = {
      callId: "",
      roomId: room.id,
      peerId,
      role: "caller",
      video,
      pendingSignals: [],
      localStream: null,
      peerConnection: null
    };
    showCallDialog(video ? "Video call" : "Audio call", "Calling…");
    if (!sendPacket({ t: "call_invite", roomId: room.id, calleeId: peerId, video })) {
      endCall(false);
      return;
    }
    byId("hangup-call-button").hidden = false;
  }

  byId("audio-call-button").addEventListener("click", () => startCall(false));
  byId("video-call-button").addEventListener("click", () => startCall(true));
  byId("accept-call-button").addEventListener("click", () => {
    if (!callState) return;
    byId("accept-call-button").hidden = true;
    byId("decline-call-button").hidden = true;
    byId("call-status").textContent = "Joining call…";
    sendPacket({ t: "call_accept", callId: callState.callId });
  });
  byId("decline-call-button").addEventListener("click", () => {
    if (callState?.callId) sendPacket({ t: "call_decline", callId: callState.callId });
    endCall(false);
  });
  byId("hangup-call-button").addEventListener("click", () => endCall(true));
  byId("close-call-button").addEventListener("click", () => endCall(true));
  byId("mute-call-button").addEventListener("click", () => {
    const tracks = callState?.localStream?.getAudioTracks() || [];
    const enabled = tracks.some((track) => track.enabled);
    for (const track of tracks) track.enabled = !enabled;
    byId("mute-call-button").textContent = enabled ? "Unmute" : "Mute";
  });
  byId("call-dialog").addEventListener("cancel", (event) => {
    event.preventDefault();
    endCall(true);
  });

  byId("signout-button").addEventListener("click", () => {
    if (callState) endCall(true);
    sendPacket({ t: "logout", token: localStorage.getItem(tokenKey) || "" });
  });

  connect();
})();