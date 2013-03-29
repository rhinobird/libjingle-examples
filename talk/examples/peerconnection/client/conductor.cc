/*
 * libjingle
 * Copyright 2012, Google Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "talk/examples/peerconnection/client/conductor.h"

#include <utility>

#include "talk/app/webrtc/videosourceinterface.h"
#include "talk/base/common.h"
#include "talk/base/json.h"
#include "talk/base/logging.h"
#include "talk/examples/peerconnection/client/defaults.h"
#include "talk/media/devices/devicemanager.h"
#include "talk/media/base/videocapturer.h"
#include "talk/media/base/videorenderer.h"

// Names used for a IceCandidate JSON object.
const char kCandidateSdpMidName[] = "sdpMid";
const char kCandidateSdpMlineIndexName[] = "sdpMLineIndex";
const char kCandidateSdpName[] = "candidate";

// Names used for a SessionDescription JSON object.
const char kSessionDescriptionTypeName[] = "type";
const char kSessionDescriptionSdpName[] = "sdp";
//AF CODE

class DummyVideoCapturer : public cricket::VideoCapturer {
  public:
    DummyVideoCapturer () : cricket::VideoCapturer () {}

    cricket::CaptureState Start(const cricket::VideoFormat& capture_format) {return cricket::CS_RUNNING;}
    void Stop() {}
    bool IsRunning() {return true;}
    bool IsScreencast() const {return true;}
    bool GetPreferredFourccs(std::vector<uint32>* fourccs) {return true;}
  };

class DummyVideoRenderer : public webrtc::VideoRendererInterface {
   public:
    DummyVideoRenderer(DummyVideoCapturer* main_wnd,
                  webrtc::VideoTrackInterface* track_to_render);
    virtual ~DummyVideoRenderer();

    // VideoRendererInterface implementation
    virtual void SetSize(int width, int height);
    virtual void RenderFrame(const cricket::VideoFrame* frame);

    const uint8* image() const {
      return image_.get();
    }

    int width() const {
      return width_;
    }

    int height() const {
      return height_;
    }

   protected:
    talk_base::scoped_array<uint8> image_;
    int width_;
    int height_;
    DummyVideoCapturer* main_wnd_;
    talk_base::scoped_refptr<webrtc::VideoTrackInterface> rendered_track_;
  };

DummyVideoRenderer::DummyVideoRenderer(
    DummyVideoCapturer* main_wnd,
    webrtc::VideoTrackInterface* track_to_render)
    : width_(0),
      height_(0),
      main_wnd_(main_wnd),
      rendered_track_(track_to_render) {
  rendered_track_->AddRenderer(this);
}

DummyVideoRenderer::~DummyVideoRenderer() {
  rendered_track_->RemoveRenderer(this);
}

void DummyVideoRenderer::SetSize(int width, int height) {
  #if 0
  gdk_threads_enter();
  width_ = width;
  height_ = height;
  image_.reset(new uint8[width * height * 4]);
  gdk_threads_leave();
  #endif
}

void DummyVideoRenderer::RenderFrame(const cricket::VideoFrame* frame) {
  #if 0
  gdk_threads_enter();

  int size = width_ * height_ * 4;
  // TODO: Convert directly to RGBA
  frame->ConvertToRgbBuffer(cricket::FOURCC_ARGB,
                            image_.get(),
                            size,
                            width_ * 4);
  // Convert the B,G,R,A frame to R,G,B,A, which is accepted by GTK.
  // The 'A' is just padding for GTK, so we can use it as temp.
  uint8* pix = image_.get();
  uint8* end = image_.get() + size;
  while (pix < end) {
    pix[3] = pix[0];     // Save B to A.
    pix[0] = pix[2];  // Set Red.
    pix[2] = pix[3];  // Set Blue.
    pix[3] = 0xFF;     // Fixed Alpha.
    pix += 4;
  }

  gdk_threads_leave();

  g_idle_add(Redraw, main_wnd_);
  #endif
  main_wnd_->SignalVideoFrame(main_wnd_, frame);
}

talk_base::scoped_ptr<DummyVideoRenderer> remote_renderer_;
DummyVideoCapturer *fake_capt_;

talk_base::scoped_refptr<webrtc::MediaStreamInterface> stream_;
class DummySetSessionDescriptionObserver
    : public webrtc::SetSessionDescriptionObserver {
 public:
  static DummySetSessionDescriptionObserver* Create() {
    return
        new talk_base::RefCountedObject<DummySetSessionDescriptionObserver>();
  }
  virtual void OnSuccess() {
    LOG(INFO) << __FUNCTION__;
  }
  virtual void OnFailure(const std::string& error) {
    LOG(INFO) << __FUNCTION__ << " " << error;
  }

 protected:
  DummySetSessionDescriptionObserver() {}
  ~DummySetSessionDescriptionObserver() {}
};

Conductor::Conductor(PeerConnectionClient* client, MainWindow* main_wnd)
  : peer_id_(-1),
    client_(client),
    main_wnd_(main_wnd) {
  client_->RegisterObserver(this);
  main_wnd->RegisterObserver(this);
  fake_capt_ = new DummyVideoCapturer();
}

Conductor::~Conductor() {
  ASSERT(peer_connection_.get() == NULL);
}

bool Conductor::connection_active() const {
  return peer_connection_.get() != NULL;
}

void Conductor::Close() {
  client_->SignOut();
  DeletePeerConnection();
}

bool Conductor::InitializePeerConnection() {
  ASSERT(peer_connection_factory_.get() == NULL);
  ASSERT(peer_connection_.get() == NULL);

  peer_connection_factory_  = webrtc::CreatePeerConnectionFactory();

  if (!peer_connection_factory_.get()) {
    main_wnd_->MessageBox("Error",
        "Failed to initialize PeerConnectionFactory", true);
    DeletePeerConnection();
    return false;
  }

  webrtc::PeerConnectionInterface::IceServers servers;
  webrtc::PeerConnectionInterface::IceServer server;
  server.uri = GetPeerConnectionString();
  servers.push_back(server);
  peer_connection_ = peer_connection_factory_->CreatePeerConnection(servers,
                                                                    NULL,
                                                                    this);
  if (!peer_connection_.get()) {
    main_wnd_->MessageBox("Error",
        "CreatePeerConnection failed", true);
    DeletePeerConnection();
  }
  //EL CLIENTE QUE NO MANDA...
  //AddStreams();
  return peer_connection_.get() != NULL;
}

void Conductor::DeletePeerConnection() {
  peer_connection_ = NULL;
  active_streams_.clear();
  main_wnd_->StopLocalRenderer();
  main_wnd_->StopRemoteRenderer();
  peer_connection_factory_ = NULL;
  peer_id_ = -1;
}

void Conductor::EnsureStreamingUI() {
  ASSERT(peer_connection_.get() != NULL);
  if (main_wnd_->IsWindow()) {
    if (main_wnd_->current_ui() != MainWindow::STREAMING)
      main_wnd_->SwitchToStreamingUI();
  }
}

//
// PeerConnectionObserver implementation.
//

void Conductor::OnError() {
  LOG(LS_ERROR) << __FUNCTION__;
  main_wnd_->QueueUIThreadCallback(PEER_CONNECTION_ERROR, NULL);
}

// Called when a remote stream is added
//### CONDUCTORS STREAM HANDLER CHANGES TO
/*void Conductor::OnAddStream(webrtc::MediaStreamInterface* stream) {
  LOG(INFO) << __FUNCTION__ << " " << stream->label();

  stream->AddRef();
  main_wnd_->QueueUIThreadCallback(NEW_STREAM_ADDED,
                                   stream);
}*/
void Conductor::OnAddStream(webrtc::MediaStreamInterface* stream) {
  LOG(INFO) << __FUNCTION__ << " " << stream->label();
  // Create Local Stream

  if (stream_ == NULL)
    stream_ = peer_connection_factory_->CreateLocalMediaStream(kStreamLabel);
 
LOG(INFO) << "created a " << stream_->label();

  if (stream_->GetAudioTracks().size() == 0)
  {
     LOG(INFO) << "HEELLO i can get the audiotracks";

    webrtc::AudioTrackVector audio_tracks = stream->GetAudioTracks();
    webrtc::VideoTrackVector video_tracks = stream->GetVideoTracks();
    //add audio
    //stream_->AddTrack (audio_tracks[0]);
LOG(INFO) << "HEELLO i can add the audio track!";
    //stream->AddRef();
    // make local video track connected with fake capturer
    // talk_base::scoped_refptr<webrtc::VideoTrackInterface> video_track(
    //     peer_connection_factory_->CreateVideoTrack(
    //         kVideoLabel, fake_capt_));

    talk_base::scoped_refptr<webrtc::VideoTrackInterface> video_track(
          peer_connection_factory_->CreateVideoTrack(
              kVideoLabel,
              peer_connection_factory_->CreateVideoSource(fake_capt_,
                                                          NULL)));
LOG(INFO) << "created the video track";

    // make local renderer connected with fake capturer
    remote_renderer_.reset(new DummyVideoRenderer(fake_capt_, video_tracks[0]));
    stream_->AddTrack (video_track);

    LOG(INFO) << "added the video track!";

    //stream_->AddRef ();
    peer_connection_->AddStream (stream_, NULL);
  }
  else
  {
    LOG(INFO) << "there where audio tracks! :S";
  }
//MIGHT BE SOME CODE MISSING?
}

void Conductor::OnRemoveStream(webrtc::MediaStreamInterface* stream) {
  LOG(INFO) << __FUNCTION__ << " " << stream->label();
  stream->AddRef();
  main_wnd_->QueueUIThreadCallback(STREAM_REMOVED,
                                   stream);
}

void Conductor::OnIceCandidate(const webrtc::IceCandidateInterface* candidate) {
  LOG(INFO) << __FUNCTION__ << " " << candidate->sdp_mline_index();
  Json::StyledWriter writer;
  Json::Value jmessage;

  jmessage[kCandidateSdpMidName] = candidate->sdp_mid();
  jmessage[kCandidateSdpMlineIndexName] = candidate->sdp_mline_index();
  std::string sdp;
  if (!candidate->ToString(&sdp)) {
    LOG(LS_ERROR) << "Failed to serialize candidate";
    return;
  }
  jmessage[kCandidateSdpName] = sdp;
  SendMessage(writer.write(jmessage));
}

//
// PeerConnectionClientObserver implementation.
//

void Conductor::OnSignedIn() {
  LOG(INFO) << __FUNCTION__;
  main_wnd_->SwitchToPeerList(client_->peers());
}

void Conductor::OnDisconnected() {
  LOG(INFO) << __FUNCTION__;

  DeletePeerConnection();

  if (main_wnd_->IsWindow())
    main_wnd_->SwitchToConnectUI();
}

void Conductor::OnPeerConnected(int id, const std::string& name) {
  LOG(INFO) << __FUNCTION__;
  // Refresh the list if we're showing it.
  if (main_wnd_->current_ui() == MainWindow::LIST_PEERS)
    main_wnd_->SwitchToPeerList(client_->peers());
}

void Conductor::OnPeerDisconnected(int id) {
  LOG(INFO) << __FUNCTION__;
  if (id == peer_id_) {
    LOG(INFO) << "Our peer disconnected";
    main_wnd_->QueueUIThreadCallback(PEER_CONNECTION_CLOSED, NULL);
  } else {
    // Refresh the list if we're showing it.
    if (main_wnd_->current_ui() == MainWindow::LIST_PEERS)
      main_wnd_->SwitchToPeerList(client_->peers());
  }
}

void Conductor::OnMessageFromPeer(int peer_id, const std::string& message) {
  ASSERT(peer_id_ == peer_id || peer_id_ == -1);
  ASSERT(!message.empty());

  if (!peer_connection_.get()) {
    ASSERT(peer_id_ == -1);
    peer_id_ = peer_id;

    if (!InitializePeerConnection()) {
      LOG(LS_ERROR) << "Failed to initialize our PeerConnection instance";
      client_->SignOut();
      return;
    }
  } else if (peer_id != peer_id_) {
    ASSERT(peer_id_ != -1);
    LOG(WARNING) << "Received a message from unknown peer while already in a "
                    "conversation with a different peer.";
    return;
  }

  Json::Reader reader;
  Json::Value jmessage;
  if (!reader.parse(message, jmessage)) {
    LOG(WARNING) << "Received unknown message. " << message;
    return;
  }
  std::string type;
  std::string json_object;

  GetStringFromJsonObject(jmessage, kSessionDescriptionTypeName, &type);
  if (!type.empty()) {
    std::string sdp;
    if (!GetStringFromJsonObject(jmessage, kSessionDescriptionSdpName, &sdp)) {
      LOG(WARNING) << "Can't parse received session description message.";
      return;
    }
    webrtc::SessionDescriptionInterface* session_description(
        webrtc::CreateSessionDescription(type, sdp));
    if (!session_description) {
      LOG(WARNING) << "Can't parse received session description message.";
      return;
    }
    LOG(INFO) << " Received session description :" << message;
    peer_connection_->SetRemoteDescription(
        DummySetSessionDescriptionObserver::Create(), session_description);
    if (session_description->type() ==
        webrtc::SessionDescriptionInterface::kOffer) {
      peer_connection_->CreateAnswer(this, NULL);
    }
    return;
  } else {
    std::string sdp_mid;
    int sdp_mlineindex = 0;
    std::string sdp;
    if (!GetStringFromJsonObject(jmessage, kCandidateSdpMidName, &sdp_mid) ||
        !GetIntFromJsonObject(jmessage, kCandidateSdpMlineIndexName,
                              &sdp_mlineindex) ||
        !GetStringFromJsonObject(jmessage, kCandidateSdpName, &sdp)) {
      LOG(WARNING) << "Can't parse received message.";
      return;
    }
    talk_base::scoped_ptr<webrtc::IceCandidateInterface> candidate(
        webrtc::CreateIceCandidate(sdp_mid, sdp_mlineindex, sdp));
    if (!candidate.get()) {
      LOG(WARNING) << "Can't parse received candidate message.";
      return;
    }
    if (!peer_connection_->AddIceCandidate(candidate.get())) {
      LOG(WARNING) << "Failed to apply the received candidate";
      return;
    }
    LOG(INFO) << " Received candidate :" << message;
    return;
  }
}

void Conductor::OnMessageSent(int err) {
  // Process the next pending message if any.
  main_wnd_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, NULL);
}

void Conductor::OnServerConnectionFailure() {
    main_wnd_->MessageBox("Error", ("Failed to connect to " + server_).c_str(),
                          true);
}

//
// MainWndCallback implementation.
//

void Conductor::StartLogin(const std::string& server, int port) {
  if (client_->is_connected())
    return;
  server_ = server;
  client_->Connect(server, port, GetPeerName());
}

void Conductor::DisconnectFromServer() {
  if (client_->is_connected())
    client_->SignOut();
}

void Conductor::ConnectToPeer(int peer_id) {
  ASSERT(peer_id_ == -1);
  ASSERT(peer_id != -1);

  if (peer_connection_.get()) {
    main_wnd_->MessageBox("Error",
        "We only support connecting to one peer at a time", true);
    return;
  }

  if (InitializePeerConnection()) {
    peer_id_ = peer_id;
    peer_connection_->CreateOffer(this, NULL);
  } else {
    main_wnd_->MessageBox("Error", "Failed to initialize PeerConnection", true);
  }
}

cricket::VideoCapturer* Conductor::OpenVideoCaptureDevice() {
  talk_base::scoped_ptr<cricket::DeviceManagerInterface> dev_manager(
      cricket::DeviceManagerFactory::Create());
  if (!dev_manager->Init()) {
    LOG(LS_ERROR) << "Can't create device manager";
    return NULL;
  }
  std::vector<cricket::Device> devs;
  if (!dev_manager->GetVideoCaptureDevices(&devs)) {
    LOG(LS_ERROR) << "Can't enumerate video devices";
    return NULL;
  }
  std::vector<cricket::Device>::iterator dev_it = devs.begin();
  cricket::VideoCapturer* capturer = NULL;
  for (; dev_it != devs.end(); ++dev_it) {
    capturer = dev_manager->CreateVideoCapturer(*dev_it);
    if (capturer != NULL)
      break;
  }
  return capturer;
}

void Conductor::AddStreams() {
  if (active_streams_.find(kStreamLabel) != active_streams_.end())
    return;  // Already added.

  talk_base::scoped_refptr<webrtc::AudioTrackInterface> audio_track(
      peer_connection_factory_->CreateAudioTrack(
          kAudioLabel, peer_connection_factory_->CreateAudioSource(NULL)));

  talk_base::scoped_refptr<webrtc::VideoTrackInterface> video_track(
      peer_connection_factory_->CreateVideoTrack(
          kVideoLabel,
          peer_connection_factory_->CreateVideoSource(OpenVideoCaptureDevice(),
                                                      NULL)));
  main_wnd_->StartLocalRenderer(video_track);

  talk_base::scoped_refptr<webrtc::MediaStreamInterface> stream =
      peer_connection_factory_->CreateLocalMediaStream(kStreamLabel);

  stream->AddTrack(audio_track);
  stream->AddTrack(video_track);
  if (!peer_connection_->AddStream(stream, NULL)) {
    LOG(LS_ERROR) << "Adding stream to PeerConnection failed";
  }
  typedef std::pair<std::string,
                    talk_base::scoped_refptr<webrtc::MediaStreamInterface> >
      MediaStreamPair;
  active_streams_.insert(MediaStreamPair(stream->label(), stream));
  main_wnd_->SwitchToStreamingUI();
}

void Conductor::DisconnectFromCurrentPeer() {
  LOG(INFO) << __FUNCTION__;
  if (peer_connection_.get()) {
    client_->SendHangUp(peer_id_);
    DeletePeerConnection();
  }

  if (main_wnd_->IsWindow())
    main_wnd_->SwitchToPeerList(client_->peers());
}

void Conductor::UIThreadCallback(int msg_id, void* data) {
  switch (msg_id) {
    case PEER_CONNECTION_CLOSED:
      LOG(INFO) << "PEER_CONNECTION_CLOSED";
      DeletePeerConnection();

      ASSERT(active_streams_.empty());

      if (main_wnd_->IsWindow()) {
        if (client_->is_connected()) {
          main_wnd_->SwitchToPeerList(client_->peers());
        } else {
          main_wnd_->SwitchToConnectUI();
        }
      } else {
        DisconnectFromServer();
      }
      break;

    case SEND_MESSAGE_TO_PEER: {
      LOG(INFO) << "SEND_MESSAGE_TO_PEER";
      std::string* msg = reinterpret_cast<std::string*>(data);
      if (msg) {
        // For convenience, we always run the message through the queue.
        // This way we can be sure that messages are sent to the server
        // in the same order they were signaled without much hassle.
        pending_messages_.push_back(msg);
      }

      if (!pending_messages_.empty() && !client_->IsSendingMessage()) {
        msg = pending_messages_.front();
        pending_messages_.pop_front();

        if (!client_->SendToPeer(peer_id_, *msg) && peer_id_ != -1) {
          LOG(LS_ERROR) << "SendToPeer failed";
          DisconnectFromServer();
        }
        delete msg;
      }

      if (!peer_connection_.get())
        peer_id_ = -1;

      break;
    }

    case PEER_CONNECTION_ERROR:
      main_wnd_->MessageBox("Error", "an unknown error occurred", true);
      break;

    case NEW_STREAM_ADDED: {
      webrtc::MediaStreamInterface* stream =
          reinterpret_cast<webrtc::MediaStreamInterface*>(
          data);
      webrtc::VideoTrackVector tracks = stream->GetVideoTracks();
      // Only render the first track.
      if (!tracks.empty()) {
        webrtc::VideoTrackInterface* track = tracks[0];
        main_wnd_->StartRemoteRenderer(track);
      }
      stream->Release();
      break;
    }

    case STREAM_REMOVED: {
      // Remote peer stopped sending a stream.
      webrtc::MediaStreamInterface* stream =
          reinterpret_cast<webrtc::MediaStreamInterface*>(
          data);
      stream->Release();
      break;
    }

    default:
      ASSERT(false);
      break;
  }
}

void Conductor::OnSuccess(webrtc::SessionDescriptionInterface* desc) {
  peer_connection_->SetLocalDescription(
      DummySetSessionDescriptionObserver::Create(), desc);
  Json::StyledWriter writer;
  Json::Value jmessage;
  jmessage[kSessionDescriptionTypeName] = desc->type();
  std::string sdp;
  desc->ToString(&sdp);
  jmessage[kSessionDescriptionSdpName] = sdp;
  SendMessage(writer.write(jmessage));
}

void Conductor::OnFailure(const std::string& error) {
    LOG(LERROR) << error;
}

void Conductor::SendMessage(const std::string& json_object) {
  std::string* msg = new std::string(json_object);
  main_wnd_->QueueUIThreadCallback(SEND_MESSAGE_TO_PEER, msg);
}
