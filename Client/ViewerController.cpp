#include "ViewerController.h"
#include "ImageControl.h"
#include "Logging.h"
#include "ClientNetworkDriver.h"
#include "Clipboard.h"
#include "IClientDriver.h"
#include "Configs.h"
#include "IWebSocket.h"

#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_PNG_Image.H>

#include <chrono>
#include <mutex>
#include <assert.h>


namespace SL {
	namespace RAT {

		class ViewerControllerImpl : public Fl_Double_Window, IClientDriver {
		public:
			ImageControl* ImageControl_ = nullptr;

			Fl_Scroll* Fl_Scroll_ = nullptr;
			std::shared_ptr<Client_Config> _Config;

			std::unique_ptr<Clipboard> Clipboard_;
			ClientNetworkDriver _ClientNetworkDriver;

			std::chrono::time_point<std::chrono::steady_clock> NetworkStatsTimer_;
			std::chrono::time_point<std::chrono::steady_clock> FrameTimer_;
			float MaxFPS = 30.0f;
			int FPS = 0;
			int FrameCounter = 0;

			bool BeingClosed_ = false;
			bool CursorHidden_ = false;
			char Title_[255];
			bool HasFocus_ = false;
			std::shared_ptr<IWebSocket> Socket_;
			SocketStats LastStats;


			static void window_cb(Fl_Widget *widget, void *)
			{
				auto wnd = (ViewerControllerImpl*)widget;
				wnd->Close();
			}
			ViewerControllerImpl(std::shared_ptr<Client_Config> config) :
				Fl_Double_Window(900, 700, "Remote Host"),
				_Config(config),
				_ClientNetworkDriver(this)
			{
				FrameTimer_ = NetworkStatsTimer_ = std::chrono::steady_clock::now();
				callback(window_cb);
				Fl_Scroll_ = new Fl_Scroll(0, 0, 900, 700);

				ScreenImageInfo info;
				info.get_Height = [this]() {return this->h(); };
				info.get_Left = [this]() {return Fl_Scroll_->xposition(); };
				info.get_Top = [this]() {return Fl_Scroll_->yposition(); };
				info.OnKey = [this](int e, Press p) { this->handle_key(e, p); };
				info.OnMouse = [this](int e, int button, Press press, int x, int y) { this->handle_mouse(e, button, press, x, y); };

				ImageControl_ = new ImageControl(0, 0, 900, 700, nullptr, std::move(info));
				Fl_Scroll_->end();
				end();
				resizable(this);
				show();

				Clipboard_ = std::make_unique<Clipboard>();
				Clipboard_->shareClipboard(config->Share_Clipboard);
				Clipboard_->onChange([&](const char* c, int len) { _ClientNetworkDriver.SendClipboardText(c, static_cast<unsigned int>(len)); });

			}
			virtual ~ViewerControllerImpl() {
				if (Socket_) Socket_->close(1000, "", 0);
				Clipboard_.reset();//need to manually do this to avoid a possible race condition with the captured reference to _ClientNetworkDriver
			}
			virtual void resize(int X, int Y, int W, int H) override {
				Fl_Double_Window::resize(X, Y, W, H);

				ImageControl_->OnResize(W, H, Fl_Scroll_->scrollbar_size());
			}
			void handle_key(int e, Press press) {
				UNUSED(e);
				auto key = Fl::event_key();
				auto text = Fl::event_text();
				auto len = Fl::event_length();
				auto t = *text;
				SL_RAT_LOG(INFO_log_level, "key: '" << key << "' text: '" << text << "' len: '" << len << "'");
				//make sure to map all keys to their lower case equivelents. 
				if (t >= 'A' && t < 'Z') key = static_cast<unsigned int>(t + ('a' - 'A'));

				KeyEvent k;
				k.Key = key;
				k.PressData = press;
				k.SpecialKey = Specials::NO_SPECIAL_DATA;
				_ClientNetworkDriver.SendKey(k);
			}
			void handle_mouse(int e, int button, Press press, int x, int y) {
				MouseEvent ev;
				ev.Pos = Point(x, y);
				if (e == FL_MOUSEWHEEL) {
					ev.ScrollDelta = Fl::event_dy();
					ev.EventData = Events::SCROLL;
				}
				else {
					ev.ScrollDelta = 0;
					switch (button) {
					case FL_LEFT_MOUSE:
						ev.EventData = Events::LEFT;
						break;
					case FL_MIDDLE_MOUSE:
						ev.EventData = Events::MIDDLE;
						break;
					case FL_RIGHT_MOUSE:
						ev.EventData = Events::RIGHT;
						break;
					default:
						ev.EventData = Events::NO_EVENTDATA;
						break;
					};
				}
				ev.PressData = press;
				_ClientNetworkDriver.SendMouse(ev);
			}
			void Close() {
				if (!BeingClosed_) {
					this->hide();
					Fl::delete_widget(this);

				}
				BeingClosed_ = true;
			}
			static void closethiswindow(void* data) {
				((ViewerControllerImpl*)data)->Close();
			}
			static void awakenredraw(void* data) {
				auto imp = ((ViewerControllerImpl*)data);
				if (imp->BeingClosed_) return;
				if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - imp->FrameTimer_).count() > imp->MaxFPS / 1000.0f) {
					imp->FrameTimer_ = std::chrono::steady_clock::now();
					imp->FrameCounter += 1;
					if (!imp->BeingClosed_) imp->redraw();
				}
			}
			static void awakensettitle(void* data) {
				auto imp = ((ViewerControllerImpl*)data);
				if (imp->BeingClosed_) return;
				imp->label(imp->Title_);
			}
			static void awakenhidecursor(void* data) {
				auto imp = ((ViewerControllerImpl*)data);
				imp->cursor(Fl_Cursor::FL_CURSOR_NONE);
			}
			virtual void onReceive_Monitors(const Screen_Capture::Monitor* monitors, int num_of_monitors) override {

			}
			virtual void onConnection(const std::shared_ptr<IWebSocket>& socket) override {
				Socket_ = socket;
			}

			virtual void onDisconnection(const IWebSocket& socket, int code, char* message, size_t length) override {
				UNUSED(socket);
				Fl::awake(closethiswindow, this);
			}

			virtual void onMessage(const IWebSocket& socket, const char* data, size_t len)  override {
				UNUSED(socket);
				UNUSED(data);
				UNUSED(len);
			}


			virtual void onReceive_Image(const Image& img, int monitor_id) override
			{
				ImageControl_->set_ScreenImage(img, monitor_id);
				Fl::awake(awakenredraw, this);
			}

			virtual void onReceive_ImageDif(const Image& img, int monitor_id) override {
				ImageControl_->set_ImageDifference(img, monitor_id);
				Fl::awake(awakenredraw, this);
				if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - NetworkStatsTimer_).count() > 1000) {
					NetworkStatsTimer_ = std::chrono::steady_clock::now();

					auto stats = Socket_->get_Stats();
					std::string st = "Client ";
					st += std::to_string((stats->TotalBytesReceived - LastStats.TotalBytesReceived) / 1000) + " Kbs Received ";
					st += std::to_string((stats->TotalBytesSent - LastStats.TotalBytesSent) / 1000) + " Kbs Sent";
					FPS = FrameCounter;
					FrameCounter = 0;
					st += " Fps: " + std::to_string(FPS);
					LastStats = *stats;
					if (st.size() > sizeof(Title_) - 1) st = st.substr(0, sizeof(Title_) - 1);
					memcpy(Title_, st.c_str(), st.size() + 1);
					Fl::awake(awakensettitle, this);
				}

			}
			virtual void onReceive_MouseImage(const Image& img)override {
				ImageControl_->set_MouseImage(img);
			}

			virtual void onReceive_MousePos(const Point* pos)override {
				ImageControl_->set_MousePosition(pos);
			}
			virtual void  onReceive_ClipboardText(const char* data, unsigned int len) override {
				Clipboard_->updateClipbard(data, len);
			}


		};
	}
}


SL::RAT::ViewerController::ViewerController(std::shared_ptr<Client_Config> config, const char * dst_host) {

	_ViewerControllerImpl = new ViewerControllerImpl(config);
	_ViewerControllerImpl->_ClientNetworkDriver.Connect(config, dst_host);
}

SL::RAT::ViewerController::~ViewerController() {

}
