/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "inline_bots/bot_attach_web_view.h"

#include "data/data_user.h"
#include "data/data_file_origin.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_session.h"
#include "main/main_session.h"
#include "main/main_domain.h"
#include "storage/storage_domain.h"
#include "info/profile/info_profile_values.h"
#include "ui/boxes/confirm_box.h"
#include "ui/toasts/common_toasts.h"
#include "ui/chat/attach/attach_bot_webview.h"
#include "ui/widgets/dropdown_menu.h"
#include "ui/widgets/popup_menu.h"
#include "ui/widgets/menu/menu_item_base.h"
#include "ui/text/text_utilities.h"
#include "ui/effects/ripple_animation.h"
#include "window/themes/window_theme.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "webview/webview_interface.h"
#include "core/application.h"
#include "core/local_url_handlers.h"
#include "ui/basic_click_handlers.h"
#include "history/history.h"
#include "history/history_item.h"
#include "storage/storage_account.h"
#include "lang/lang_keys.h"
#include "base/random.h"
#include "base/timer_rpl.h"
#include "apiwrap.h"
#include "styles/style_menu_icons.h"

#include <QSvgRenderer>

namespace InlineBots {
namespace {

constexpr auto kProlongTimeout = 60 * crl::time(1000);

struct ParsedBot {
	UserData *bot = nullptr;
	bool inactive = false;
};

[[nodiscard]] DocumentData *ResolveIcon(
		not_null<Main::Session*> session,
		const MTPDattachMenuBot &data) {
	for (const auto &icon : data.vicons().v) {
		const auto document = icon.match([&](
			const MTPDattachMenuBotIcon &data
		) -> DocumentData* {
			if (data.vname().v == "default_static") {
				return session->data().processDocument(data.vicon()).get();
			}
			return nullptr;
		});
		if (document) {
			return document;
		}
	}
	return nullptr;
}

[[nodiscard]] std::optional<AttachWebViewBot> ParseAttachBot(
		not_null<Main::Session*> session,
		const MTPAttachMenuBot &bot) {
	auto result = bot.match([&](const MTPDattachMenuBot &data) {
		const auto user = session->data().userLoaded(UserId(data.vbot_id()));
		const auto good = user
			&& user->isBot()
			&& user->botInfo->supportsAttachMenu;
		return good
			? AttachWebViewBot{
				.user = user,
				.icon = ResolveIcon(session, data),
				.name = qs(data.vshort_name()),
				.inactive = data.is_inactive(),
			} : std::optional<AttachWebViewBot>();
	});
	if (result && result->icon) {
		result->icon->forceToCache(true);
	}
	return result;
}

[[nodiscard]] base::flat_set<not_null<AttachWebView*>> &ActiveWebViews() {
	static auto result = base::flat_set<not_null<AttachWebView*>>();
	return result;
}

class BotAction final : public Ui::Menu::ItemBase {
public:
	BotAction(
		not_null<Ui::RpWidget*> parent,
		const style::Menu &st,
		const AttachWebViewBot &bot,
		Fn<void()> callback);

	bool isEnabled() const override;
	not_null<QAction*> action() const override;

	[[nodiscard]] rpl::producer<bool> forceShown() const;

	void handleKeyPress(not_null<QKeyEvent*> e) override;

private:
	void contextMenuEvent(QContextMenuEvent *e) override;

	QPoint prepareRippleStartPosition() const override;
	QImage prepareRippleMask() const override;

	int contentHeight() const override;

	void prepare();
	void validateIcon();
	void paint(Painter &p);

	const not_null<QAction*> _dummyAction;
	const style::Menu &_st;
	const AttachWebViewBot _bot;

	base::unique_qptr<Ui::PopupMenu> _menu;
	rpl::event_stream<bool> _forceShown;

	Ui::Text::String _text;
	QImage _mask;
	QImage _icon;
	int _textWidth = 0;
	const int _height;

};

BotAction::BotAction(
	not_null<Ui::RpWidget*> parent,
	const style::Menu &st,
	const AttachWebViewBot &bot,
	Fn<void()> callback)
: ItemBase(parent, st)
, _dummyAction(new QAction(parent))
, _st(st)
, _bot(bot)
, _height(_st.itemPadding.top()
		+ _st.itemStyle.font->height
		+ _st.itemPadding.bottom()) {
	setAcceptBoth(false);
	initResizeHook(parent->sizeValue());
	setClickedCallback(std::move(callback));

	paintRequest(
	) | rpl::start_with_next([=] {
		Painter p(this);
		paint(p);
	}, lifetime());

	style::PaletteChanged(
	) | rpl::start_with_next([=] {
		_icon = QImage();
		update();
	}, lifetime());

	enableMouseSelecting();
	prepare();
}

void BotAction::validateIcon() {
	if (_mask.isNull()) {
		if (!_bot.media || !_bot.media->loaded()) {
			return;
		}
		auto icon = QSvgRenderer(_bot.media->bytes());
		if (!icon.isValid()) {
			_mask = QImage(
				QSize(1, 1) * style::DevicePixelRatio(),
				QImage::Format_ARGB32_Premultiplied);
			_mask.fill(Qt::transparent);
		} else {
			const auto size = style::ConvertScale(icon.defaultSize());
			_mask = QImage(
				size * style::DevicePixelRatio(),
				QImage::Format_ARGB32_Premultiplied);
			_mask.fill(Qt::transparent);
			{
				auto p = QPainter(&_mask);
				icon.render(&p, QRect(QPoint(), size));
			}
			_mask = Images::Colored(std::move(_mask), QColor(255, 255, 255));
		}
	}
	if (_icon.isNull()) {
		_icon = style::colorizeImage(_mask, st::menuIconColor);
	}
}

void BotAction::paint(Painter &p) {
	validateIcon();

	const auto selected = isSelected();
	if (selected && _st.itemBgOver->c.alpha() < 255) {
		p.fillRect(0, 0, width(), _height, _st.itemBg);
	}
	p.fillRect(0, 0, width(), _height, selected ? _st.itemBgOver : _st.itemBg);
	if (isEnabled()) {
		paintRipple(p, 0, 0);
	}

	if (!_icon.isNull()) {
		p.drawImage(_st.itemIconPosition, _icon);
	}

	p.setPen(selected ? _st.itemFgOver : _st.itemFg);
	_text.drawLeftElided(
		p,
		_st.itemPadding.left(),
		_st.itemPadding.top(),
		_textWidth,
		width());
}

void BotAction::prepare() {
	_text.setMarkedText(_st.itemStyle, { _bot.name });
	const auto textWidth = _text.maxWidth();
	const auto &padding = _st.itemPadding;

	const auto goodWidth = padding.left()
		+ textWidth
		+ padding.right();

	const auto w = std::clamp(goodWidth, _st.widthMin, _st.widthMax);
	_textWidth = w - (goodWidth - textWidth);
	setMinWidth(w);
	update();
}

bool BotAction::isEnabled() const {
	return true;
}

not_null<QAction*> BotAction::action() const {
	return _dummyAction;
}

void BotAction::contextMenuEvent(QContextMenuEvent *e) {
	_menu = nullptr;
	_menu = base::make_unique_q<Ui::PopupMenu>(
		this,
		st::popupMenuWithIcons);
	_menu->addAction(tr::lng_bot_remove_from_menu(tr::now), [=] {
		_bot.user->session().attachWebView().removeFromMenu(_bot.user);
	}, &st::menuIconDelete);

	QObject::connect(_menu, &QObject::destroyed, [=] {
		_forceShown.fire(false);
	});

	_forceShown.fire(true);
	_menu->popup(e->globalPos());
	e->accept();
}

QPoint BotAction::prepareRippleStartPosition() const {
	return mapFromGlobal(QCursor::pos());
}

QImage BotAction::prepareRippleMask() const {
	return Ui::RippleAnimation::rectMask(size());
}

int BotAction::contentHeight() const {
	return _height;
}

rpl::producer<bool> BotAction::forceShown() const {
	return _forceShown.events();
}

void BotAction::handleKeyPress(not_null<QKeyEvent*> e) {
	if (!isSelected()) {
		return;
	}
	const auto key = e->key();
	if (key == Qt::Key_Enter || key == Qt::Key_Return) {
		setClicked(Ui::Menu::TriggeredSource::Keyboard);
	}
}

} // namespace

AttachWebView::AttachWebView(not_null<Main::Session*> session)
: _session(session) {
}

AttachWebView::~AttachWebView() {
	ActiveWebViews().remove(this);
}

void AttachWebView::request(
		not_null<PeerData*> peer,
		const QString &botUsername,
		const QString &startCommand) {
	if (botUsername.isEmpty()) {
		return;
	}
	const auto username = _bot ? _bot->username : _botUsername;
	if (_peer == peer
		&& username.toLower() == botUsername.toLower()
		&& _startCommand == startCommand) {
		if (_panel) {
			_panel->requestActivate();
		}
		return;
	}
	cancel();

	_peer = peer;
	_botUsername = botUsername;
	_startCommand = startCommand;
	resolve();
}

void AttachWebView::request(
		Window::SessionController *controller,
		not_null<PeerData*> peer,
		not_null<UserData*> bot,
		const WebViewButton &button) {
	if (_peer == peer && _bot == bot) {
		if (_panel) {
			_panel->requestActivate();
		} else if (_requestId) {
			return;
		}
	}
	cancel();

	_bot = bot;
	_peer = peer;
	if (controller) {
		confirmOpen(controller, [=] {
			request(button);
		});
	} else {
		request(button);
	}
}

void AttachWebView::request(const WebViewButton &button) {
	Expects(_peer != nullptr && _bot != nullptr);

	_startCommand = button.startCommand;

	using Flag = MTPmessages_RequestWebView::Flag;
	const auto flags = Flag::f_theme_params
		| (button.url.isEmpty() ? Flag(0) : Flag::f_url)
		| (_startCommand.isEmpty() ? Flag(0) : Flag::f_start_param);
	_requestId = _session->api().request(MTPmessages_RequestWebView(
		MTP_flags(flags),
		_peer->input,
		_bot->inputUser,
		MTP_bytes(button.url),
		MTP_string(_startCommand),
		MTP_dataJSON(MTP_bytes(Window::Theme::WebViewParams().json)),
		MTPint() // reply_to_msg_id
	)).done([=](const MTPWebViewResult &result) {
		_requestId = 0;
		result.match([&](const MTPDwebViewResultUrl &data) {
			show(data.vquery_id().v, qs(data.vurl()), button.text);
		});
	}).fail([=](const MTP::Error &error) {
		_requestId = 0;
		if (error.type() == u"BOT_INVALID"_q) {
			requestBots();
		}
	}).send();
}

void AttachWebView::cancel() {
	ActiveWebViews().remove(this);
	_session->api().request(base::take(_requestId)).cancel();
	_session->api().request(base::take(_prolongId)).cancel();
	_panel = nullptr;
	_peer = _bot = nullptr;
	_botUsername = QString();
	_startCommand = QString();
}

void AttachWebView::requestBots() {
	if (_botsRequestId) {
		return;
	}
	_botsRequestId = _session->api().request(MTPmessages_GetAttachMenuBots(
		MTP_long(_botsHash)
	)).done([=](const MTPAttachMenuBots &result) {
		_botsRequestId = 0;
		result.match([&](const MTPDattachMenuBotsNotModified &) {
		}, [&](const MTPDattachMenuBots &data) {
			_session->data().processUsers(data.vusers());
			_botsHash = data.vhash().v;
			_attachBots.clear();
			_attachBots.reserve(data.vbots().v.size());
			for (const auto &bot : data.vbots().v) {
				if (auto parsed = ParseAttachBot(_session, bot)) {
					if (!parsed->inactive) {
						if (const auto icon = parsed->icon) {
							parsed->media = icon->createMediaView();
							icon->save(Data::FileOrigin(), {});
						}
						_attachBots.push_back(std::move(*parsed));
					}
				}
			}
			_attachBotsUpdates.fire({});
		});
	}).fail([=] {
		_botsRequestId = 0;
	}).send();
}

void AttachWebView::requestAddToMenu(
		PeerData *peer,
		not_null<UserData*> bot,
		const QString &startCommand) {
	if (!bot->isBot() || !bot->botInfo->supportsAttachMenu) {
		Ui::ShowMultilineToast({
			.text = { tr::lng_bot_menu_not_supported(tr::now) },
		});
		return;
	}
	_addToMenuStartCommand = startCommand;
	_addToMenuPeer = peer;
	if (_addToMenuId) {
		if (_addToMenuBot == bot) {
			return;
		}
		_session->api().request(base::take(_addToMenuId)).cancel();
	}
	_addToMenuBot = bot;
	_addToMenuId = _session->api().request(MTPmessages_GetAttachMenuBot(
		bot->inputUser
	)).done([=](const MTPAttachMenuBotsBot &result) {
		_addToMenuId = 0;
		const auto bot = base::take(_addToMenuBot);
		const auto contextPeer = base::take(_addToMenuPeer);
		const auto startCommand = base::take(_addToMenuStartCommand);
		const auto open = [=] {
			if (!contextPeer) {
				return false;
			}
			request(
				nullptr,
				contextPeer,
				bot,
				{ .startCommand = startCommand });
			return true;
		};
		result.match([&](const MTPDattachMenuBotsBot &data) {
			_session->data().processUsers(data.vusers());
			if (const auto parsed = ParseAttachBot(_session, data.vbot())) {
				if (bot == parsed->user) {
					if (parsed->inactive) {
						confirmAddToMenu(*parsed, open);
					} else {
						requestBots();
						if (!open()) {
							Ui::ShowMultilineToast({
								.text = {
									tr::lng_bot_menu_already_added(tr::now) },
							});
						}
					}
				}
			}
		});
	}).fail([=] {
		_addToMenuId = 0;
		_addToMenuBot = nullptr;
		_addToMenuPeer = nullptr;
		_addToMenuStartCommand = QString();
		Ui::ShowMultilineToast({
			.text = { tr::lng_bot_menu_not_supported(tr::now) },
		});
	}).send();
}

void AttachWebView::removeFromMenu(not_null<UserData*> bot) {
	toggleInMenu(bot, false, [=] {
		Ui::ShowMultilineToast({
			.text = { tr::lng_bot_remove_from_menu_done(tr::now) },
		});
	});
}

void AttachWebView::resolve() {
	resolveUsername(_botUsername, [=](not_null<PeerData*> bot) {
		_bot = bot->asUser();
		if (!_bot) {
			Ui::ShowMultilineToast({
				.text = { tr::lng_bot_menu_not_supported(tr::now) }
			});
			return;
		}
		requestAddToMenu(_peer, _bot, _startCommand);
	});
}

void AttachWebView::resolveUsername(
		const QString &username,
		Fn<void(not_null<PeerData*>)> done) {
	if (const auto peer = _peer->owner().peerByUsername(username)) {
		done(peer);
		return;
	}
	_session->api().request(base::take(_requestId)).cancel();
	_requestId = _session->api().request(MTPcontacts_ResolveUsername(
		MTP_string(username)
	)).done([=](const MTPcontacts_ResolvedPeer &result) {
		_requestId = 0;
		result.match([&](const MTPDcontacts_resolvedPeer &data) {
			_peer->owner().processUsers(data.vusers());
			_peer->owner().processChats(data.vchats());
			if (const auto peerId = peerFromMTP(data.vpeer())) {
				done(_peer->owner().peer(peerId));
			}
		});
	}).fail([=](const MTP::Error &error) {
		_requestId = 0;
		if (error.code() == 400) {
			Ui::ShowMultilineToast({
				.text = {
					tr::lng_username_not_found(tr::now, lt_user, username),
				},
			});
		}
	}).send();
}

void AttachWebView::requestSimple(
		not_null<Window::SessionController*> controller,
		not_null<UserData*> bot,
		const WebViewButton &button) {
	cancel();
	_bot = bot;
	_peer = bot;
	confirmOpen(controller, [=] {
		requestSimple(button);
	});
}

void AttachWebView::requestSimple(const WebViewButton &button) {
	using Flag = MTPmessages_RequestSimpleWebView::Flag;
	_requestId = _session->api().request(MTPmessages_RequestSimpleWebView(
		MTP_flags(Flag::f_theme_params),
		_bot->inputUser,
		MTP_bytes(button.url),
		MTP_dataJSON(MTP_bytes(Window::Theme::WebViewParams().json))
	)).done([=](const MTPSimpleWebViewResult &result) {
		_requestId = 0;
		result.match([&](const MTPDsimpleWebViewResultUrl &data) {
			const auto queryId = uint64();
			show(queryId, qs(data.vurl()), button.text);
		});
	}).fail([=](const MTP::Error &error) {
		_requestId = 0;
	}).send();
}

void AttachWebView::requestMenu(
		not_null<Window::SessionController*> controller,
		not_null<UserData*> bot) {
	cancel();
	_bot = bot;
	_peer = bot;
	const auto url = bot->botInfo->botMenuButtonUrl;
	const auto text = bot->botInfo->botMenuButtonText;
	confirmOpen(controller, [=] {
		using Flag = MTPmessages_RequestWebView::Flag;
		_requestId = _session->api().request(MTPmessages_RequestWebView(
			MTP_flags(Flag::f_theme_params
				| Flag::f_url
				| Flag::f_from_bot_menu),
			_bot->input,
			_bot->inputUser,
			MTP_string(url),
			MTPstring(),
			MTP_dataJSON(MTP_bytes(Window::Theme::WebViewParams().json)),
			MTPint()
		)).done([=](const MTPWebViewResult &result) {
			_requestId = 0;
			result.match([&](const MTPDwebViewResultUrl &data) {
				show(data.vquery_id().v, qs(data.vurl()), text);
			});
		}).fail([=](const MTP::Error &error) {
			_requestId = 0;
			if (error.type() == u"BOT_INVALID"_q) {
				requestBots();
			}
		}).send();
	});
}

void AttachWebView::confirmOpen(
		not_null<Window::SessionController*> controller,
		Fn<void()> done) {
	if (!_bot) {
		return;
	} else if (_bot->isVerified()
		|| _bot->session().local().isBotTrustedOpenWebView(_bot->id)) {
		done();
		return;
	}
	const auto callback = [=] {
		_bot->session().local().markBotTrustedOpenWebView(_bot->id);
		controller->hideLayer();
		done();
	};
	controller->show(Ui::MakeConfirmBox({
		.text = tr::lng_allow_bot_webview(
			tr::now,
			lt_bot_name,
			Ui::Text::Bold(_bot->name),
			Ui::Text::RichLangValue),
		.confirmed = callback,
		.confirmText = tr::lng_box_ok(),
	}));
}

void AttachWebView::ClearAll() {
	while (!ActiveWebViews().empty()) {
		ActiveWebViews().front()->cancel();
	}
}

void AttachWebView::show(
		uint64 queryId,
		const QString &url,
		const QString &buttonText) {
	Expects(_bot != nullptr && _peer != nullptr);

	const auto close = crl::guard(this, [=] {
		crl::on_main(this, [=] { cancel(); });
	});
	const auto sendData = crl::guard(this, [=](QByteArray data) {
		if (_peer != _bot || queryId) {
			return;
		}
		const auto randomId = base::RandomValue<uint64>();
		_session->api().request(MTPmessages_SendWebViewData(
			_bot->inputUser,
			MTP_long(randomId),
			MTP_string(buttonText),
			MTP_bytes(data)
		)).done([=](const MTPUpdates &result) {
			_session->api().applyUpdates(result);
		}).send();
		cancel();
	});
	const auto handleLocalUri = [close](QString uri) {
		const auto local = Core::TryConvertUrlToLocal(uri);
		if (uri == local || Core::InternalPassportLink(local)) {
			return local.startsWith(qstr("tg://"));
		} else if (!local.startsWith(qstr("tg://"), Qt::CaseInsensitive)) {
			return false;
		}
		UrlClickHandler::Open(local, {});
		close();
		return true;
	};
	auto title = Info::Profile::NameValue(
		_bot
	) | rpl::map([](const TextWithEntities &value) {
		return value.text;
	});
	ActiveWebViews().emplace(this);
	_panel = Ui::BotWebView::Show({
		.url = url,
		.userDataPath = _session->domain().local().webviewDataPath(),
		.title = std::move(title),
		.bottom = rpl::single('@' + _bot->username),
		.handleLocalUri = handleLocalUri,
		.sendData = sendData,
		.close = close,
		.themeParams = [] { return Window::Theme::WebViewParams(); },
	});
	started(queryId);
}

void AttachWebView::started(uint64 queryId) {
	Expects(_peer != nullptr && _bot != nullptr);

	_session->data().webViewResultSent(
	) | rpl::filter([=](const Data::Session::WebViewResultSent &sent) {
		return (sent.queryId == queryId);
	}) | rpl::start_with_next([=] {
		cancel();
	}, _panel->lifetime());

	base::timer_each(
		kProlongTimeout
	) | rpl::start_with_next([=] {
		using Flag = MTPmessages_ProlongWebView::Flag;
		auto flags = Flag::f_reply_to_msg_id | Flag::f_silent;
		_session->api().request(base::take(_prolongId)).cancel();
		_prolongId = _session->api().request(MTPmessages_ProlongWebView(
			MTP_flags(flags),
			_peer->input,
			_bot->inputUser,
			MTP_long(queryId),
			MTP_int(_replyToMsgId.bare)
		)).done([=] {
			_prolongId = 0;
		}).send();
	}, _panel->lifetime());
}

void AttachWebView::confirmAddToMenu(
		AttachWebViewBot bot,
		Fn<void()> callback) {
	const auto done = [=](Fn<void()> close) {
		toggleInMenu(bot.user, true, [=] {
			if (callback) {
				callback();
			}
			Ui::ShowMultilineToast({
				.text = { tr::lng_bot_add_to_menu_done(tr::now) },
			});
		});
		close();
	};
	const auto active = Core::App().activeWindow();
	if (!active) {
		return;
	}
	_confirmAddBox = active->show(Ui::MakeConfirmBox({
		tr::lng_bot_add_to_menu(tr::now, lt_bot, bot.name),
		done,
	}));
}

void AttachWebView::toggleInMenu(
		not_null<UserData*> bot,
		bool enabled,
		Fn<void()> callback) {
	_session->api().request(MTPmessages_ToggleBotInAttachMenu(
		bot->inputUser,
		MTP_bool(enabled)
	)).done([=] {
		_requestId = 0;
		requestBots();
		if (callback) {
			callback();
		}
	}).fail([=] {
		cancel();
	}).send();
}

std::unique_ptr<Ui::DropdownMenu> MakeAttachBotsMenu(
		not_null<QWidget*> parent,
		not_null<Window::SessionController*> controller,
		Fn<void(bool)> forceShown,
		Fn<void(bool)> attach) {
	auto result = std::make_unique<Ui::DropdownMenu>(
		parent,
		st::dropdownMenuWithIcons);
	const auto bots = &controller->session().attachWebView();
	const auto raw = result.get();
	const auto refresh = [=] {
		raw->clearActions();
		raw->addAction(tr::lng_attach_photo_or_video(tr::now), [=] {
			attach(true);
		}, &st::menuIconPhoto);
		raw->addAction(tr::lng_attach_document(tr::now), [=] {
			attach(false);
		}, &st::menuIconFile);
		for (const auto &bot : bots->attachBots()) {
			const auto callback = [=] {
				const auto active = controller->activeChatCurrent();
				if (const auto history = active.history()) {
					bots->request(nullptr, history->peer, bot.user, {});
				}
			};
			auto action = base::make_unique_q<BotAction>(
				raw,
				raw->menu()->st(),
				bot,
				callback);
			action->forceShown(
			) | rpl::start_with_next([=](bool shown) {
				forceShown(shown);
			}, action->lifetime());
			raw->addAction(std::move(action));
		}
	};
	refresh();
	bots->attachBotsUpdates(
	) | rpl::start_with_next(refresh, raw->lifetime());

	return result;
}

} // namespace InlineBots
