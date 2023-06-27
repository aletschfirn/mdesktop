/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_stories.h"

#include "api/api_report.h"
#include "base/unixtime.h"
#include "apiwrap.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_document.h"
#include "data/data_photo.h"
#include "data/data_user.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "main/main_session.h"
#include "ui/layers/show.h"
#include "ui/text/text_utilities.h"

namespace Data {
namespace {

constexpr auto kMaxResolveTogether = 100;
constexpr auto kIgnorePreloadAroundIfLoaded = 15;
constexpr auto kPreloadAroundCount = 30;
constexpr auto kMarkAsReadDelay = 3 * crl::time(1000);
constexpr auto kIncrementViewsDelay = 5 * crl::time(1000);
constexpr auto kArchiveFirstPerPage = 30;
constexpr auto kArchivePerPage = 100;
constexpr auto kSavedFirstPerPage = 30;
constexpr auto kSavedPerPage = 100;
constexpr auto kMaxPreloadSources = 10;
constexpr auto kStillPreloadFromFirst = 3;

using UpdateFlag = StoryUpdate::Flag;

[[nodiscard]] std::optional<StoryMedia> ParseMedia(
		not_null<Session*> owner,
		const MTPMessageMedia &media) {
	return media.match([&](const MTPDmessageMediaPhoto &data)
		-> std::optional<StoryMedia> {
		if (const auto photo = data.vphoto()) {
			const auto result = owner->processPhoto(*photo);
			if (!result->isNull()) {
				return StoryMedia{ result };
			}
		}
		return {};
	}, [&](const MTPDmessageMediaDocument &data)
		-> std::optional<StoryMedia> {
		if (const auto document = data.vdocument()) {
			const auto result = owner->processDocument(*document);
			if (!result->isNull()
				&& (result->isGifv() || result->isVideoFile())) {
				result->setStoryMedia(true);
				return StoryMedia{ result };
			}
		}
		return {};
	}, [&](const MTPDmessageMediaUnsupported &data) {
		return std::make_optional(StoryMedia{ v::null });
	}, [](const auto &) { return std::optional<StoryMedia>(); });
}

} // namespace

StoriesSourceInfo StoriesSource::info() const {
	return {
		.id = user->id,
		.last = ids.empty() ? 0 : ids.back().date,
		.unread = unread(),
		.premium = user->isPremium(),
		.hidden = hidden,
	};
}

bool StoriesSource::unread() const {
	return !ids.empty() && readTill < ids.back().id;
}

StoryIdDates StoriesSource::toOpen() const {
	if (ids.empty()) {
		return {};
	}
	const auto i = ids.lower_bound(StoryIdDates{ readTill + 1 });
	return (i != end(ids)) ? *i : ids.front();
}

Stories::Stories(not_null<Session*> owner)
: _owner(owner)
, _expireTimer([=] { processExpired(); })
, _markReadTimer([=] { sendMarkAsReadRequests(); })
, _incrementViewsTimer([=] { sendIncrementViewsRequests(); }) {
}

Stories::~Stories() {
}

Session &Stories::owner() const {
	return *_owner;
}

Main::Session &Stories::session() const {
	return _owner->session();
}

void Stories::apply(const MTPDupdateStory &data) {
	const auto peerId = peerFromUser(data.vuser_id());
	const auto user = not_null(_owner->peer(peerId)->asUser());
	const auto now = base::unixtime::now();
	const auto idDates = parseAndApply(user, data.vstory(), now);
	if (!idDates) {
		return;
	}
	const auto expired = (idDates.expires <= now);
	if (expired) {
		applyExpired({ peerId, idDates.id });
		return;
	}
	const auto i = _all.find(peerId);
	if (i == end(_all)) {
		requestUserStories(user);
		return;
	} else if (i->second.ids.contains(idDates)) {
		return;
	}
	const auto wasInfo = i->second.info();
	i->second.ids.emplace(idDates);
	const auto nowInfo = i->second.info();
	if (user->isSelf() && i->second.readTill < idDates.id) {
		i->second.readTill = idDates.id;
	}
	if (wasInfo == nowInfo) {
		return;
	}
	const auto refreshInList = [&](StorySourcesList list) {
		auto &sources = _sources[static_cast<int>(list)];
		const auto i = ranges::find(
			sources,
			peerId,
			&StoriesSourceInfo::id);
		if (i != end(sources)) {
			*i = nowInfo;
			sort(list);
		}
	};
	if (user->hasStoriesHidden()) {
		refreshInList(StorySourcesList::Hidden);
	} else {
		refreshInList(StorySourcesList::NotHidden);
	}
	_sourceChanged.fire_copy(peerId);
}

void Stories::apply(not_null<PeerData*> peer, const MTPUserStories *data) {
	if (!data) {
		applyDeletedFromSources(peer->id, StorySourcesList::NotHidden);
		applyDeletedFromSources(peer->id, StorySourcesList::Hidden);
		_all.erase(peer->id);
		_sourceChanged.fire_copy(peer->id);
	} else {
		parseAndApply(*data);
	}
}

Story *Stories::applyFromWebpage(PeerId peerId, const MTPstoryItem &story) {
	const auto idDates = parseAndApply(
		_owner->peer(peerId),
		story,
		base::unixtime::now());
	const auto value = idDates
		? lookup({ peerId, idDates.id })
		: base::make_unexpected(NoStory::Deleted);
	return value ? value->get() : nullptr;
}

void Stories::requestUserStories(not_null<UserData*> user) {
	if (!_requestingUserStories.emplace(user).second) {
		return;
	}
	_owner->session().api().request(MTPstories_GetUserStories(
		user->inputUser
	)).done([=](const MTPstories_UserStories &result) {
		_requestingUserStories.remove(user);
		const auto &data = result.data();
		_owner->processUsers(data.vusers());
		parseAndApply(data.vstories());
	}).fail([=] {
		_requestingUserStories.remove(user);
		applyDeletedFromSources(user->id, StorySourcesList::NotHidden);
		applyDeletedFromSources(user->id, StorySourcesList::Hidden);
	}).send();
}

void Stories::registerExpiring(TimeId expires, FullStoryId id) {
	for (auto i = _expiring.findFirst(expires)
		; (i != end(_expiring)) && (i->first == expires)
		; ++i) {
		if (i->second == id) {
			return;
		}
	}
	const auto reschedule = _expiring.empty()
		|| (_expiring.front().first > expires);
	_expiring.emplace(expires, id);
	if (reschedule) {
		scheduleExpireTimer();
	}
}

void Stories::scheduleExpireTimer() {
	if (_expireSchedulePosted) {
		return;
	}
	_expireSchedulePosted = true;
	crl::on_main(this, [=] {
		if (!_expireSchedulePosted) {
			return;
		}
		_expireSchedulePosted = false;
		if (_expiring.empty()) {
			_expireTimer.cancel();
		} else {
			const auto nearest = _expiring.front().first;
			const auto now = base::unixtime::now();
			const auto delay = (nearest > now)
				? (nearest - now)
				: 0;
			_expireTimer.callOnce(delay * crl::time(1000));
		}
	});
}

void Stories::processExpired() {
	const auto now = base::unixtime::now();
	auto expired = base::flat_set<FullStoryId>();
	auto i = begin(_expiring);
	for (; i != end(_expiring) && i->first <= now; ++i) {
		expired.emplace(i->second);
	}
	_expiring.erase(begin(_expiring), i);
	for (const auto &id : expired) {
		applyExpired(id);
	}
	if (!_expiring.empty()) {
		scheduleExpireTimer();
	}
}

void Stories::parseAndApply(const MTPUserStories &stories) {
	const auto &data = stories.data();
	const auto peerId = peerFromUser(data.vuser_id());
	const auto readTill = data.vmax_read_id().value_or_empty();
	const auto count = int(data.vstories().v.size());
	const auto user = _owner->peer(peerId)->asUser();
	auto result = StoriesSource{
		.user = user,
		.readTill = readTill,
		.hidden = user->hasStoriesHidden(),
	};
	const auto &list = data.vstories().v;
	const auto now = base::unixtime::now();
	result.ids.reserve(list.size());
	for (const auto &story : list) {
		if (const auto id = parseAndApply(result.user, story, now)) {
			result.ids.emplace(id);
		}
	}
	if (result.ids.empty()) {
		applyDeletedFromSources(peerId, StorySourcesList::NotHidden);
		applyDeletedFromSources(peerId, StorySourcesList::Hidden);
		return;
	} else if (user->isSelf()) {
		result.readTill = result.ids.back().id;
	}
	const auto info = result.info();
	const auto i = _all.find(peerId);
	if (i != end(_all)) {
		if (i->second != result) {
			i->second = std::move(result);
		}
	} else {
		_all.emplace(peerId, std::move(result));
	}
	const auto add = [&](StorySourcesList list) {
		auto &sources = _sources[static_cast<int>(list)];
		const auto i = ranges::find(
			sources,
			peerId,
			&StoriesSourceInfo::id);
		if (i == end(sources)) {
			sources.push_back(info);
		} else if (*i == info) {
			return;
		} else {
			*i = info;
		}
		sort(list);
	};
	if (result.user->isContact()) {
		const auto hidden = result.user->hasStoriesHidden();
		using List = StorySourcesList;
		add(hidden ? List::Hidden : List::NotHidden);
		applyDeletedFromSources(
			peerId,
			hidden ? List::NotHidden : List::Hidden);
	} else {
		applyDeletedFromSources(peerId, StorySourcesList::NotHidden);
		applyDeletedFromSources(peerId, StorySourcesList::Hidden);
	}
	_sourceChanged.fire_copy(peerId);
}

Story *Stories::parseAndApply(
		not_null<PeerData*> peer,
		const MTPDstoryItem &data,
		TimeId now) {
	const auto media = ParseMedia(_owner, data.vmedia());
	if (!media) {
		return nullptr;
	}
	const auto expires = data.vexpire_date().v;
	const auto expired = (expires <= now);
	if (expired && !data.is_pinned() && !peer->isSelf()) {
		return nullptr;
	}
	const auto id = data.vid().v;
	auto &stories = _stories[peer->id];
	const auto i = stories.find(id);
	if (i != end(stories)) {
		const auto result = i->second.get();
		const auto pinned = result->pinned();
		const auto mediaChanged = (result->media() != *media);
		if (result->applyChanges(*media, data)) {
			if (result->pinned() != pinned) {
				savedStateUpdated(result);
			}
			session().changes().storyUpdated(
				result,
				UpdateFlag::Edited);
			if (const auto item = lookupItem(result)) {
				item->applyChanges(result);
			}
		}
		if (mediaChanged) {
			const auto fullId = result->fullId();
			_preloaded.remove(fullId);
			if (_preloading && _preloading->id() == fullId) {
				_preloading = nullptr;
				rebuildPreloadSources(StorySourcesList::NotHidden);
				rebuildPreloadSources(StorySourcesList::Hidden);
				continuePreloading();
			}
		}
		return result;
	}
	const auto result = stories.emplace(id, std::make_unique<Story>(
		id,
		peer,
		StoryMedia{ *media },
		data.vdate().v,
		data.vexpire_date().v)).first->second.get();
	result->applyChanges(*media, data);
	if (result->pinned()) {
		savedStateUpdated(result);
	}

	if (peer->isSelf()) {
		const auto added = _archive.list.emplace(id).second;
		if (added) {
			if (_archiveTotal >= 0 && id > _archiveLastId) {
				++_archiveTotal;
			}
			_archiveChanged.fire({});
		}
	}

	if (expired) {
		_expiring.remove(expires, result->fullId());
		applyExpired(result->fullId());
	} else {
		registerExpiring(expires, result->fullId());
	}

	return result;
}

StoryIdDates Stories::parseAndApply(
		not_null<PeerData*> peer,
		const MTPstoryItem &story,
		TimeId now) {
	return story.match([&](const MTPDstoryItem &data) {
		if (const auto story = parseAndApply(peer, data, now)) {
			return story->idDates();
		}
		applyDeleted({ peer->id, data.vid().v });
		return StoryIdDates();
	}, [&](const MTPDstoryItemSkipped &data) {
		const auto expires = data.vexpire_date().v;
		const auto expired = (expires <= now);
		const auto fullId = FullStoryId{ peer->id, data.vid().v };
		if (!expired) {
			registerExpiring(expires, fullId);
		} else if (!peer->isSelf()) {
			applyDeleted(fullId);
			return StoryIdDates();
		} else {
			_expiring.remove(expires, fullId);
			applyExpired(fullId);
		}
		return StoryIdDates{
			data.vid().v,
			data.vdate().v,
			data.vexpire_date().v,
		};
	}, [&](const MTPDstoryItemDeleted &data) {
		applyDeleted({ peer->id, data.vid().v });
		return StoryIdDates();
	});
}

void Stories::updateDependentMessages(not_null<Data::Story*> story) {
	const auto i = _dependentMessages.find(story);
	if (i != end(_dependentMessages)) {
		for (const auto &dependent : i->second) {
			dependent->updateDependencyItem();
		}
	}
	session().changes().storyUpdated(
		story,
		Data::StoryUpdate::Flag::Edited);
}

void Stories::registerDependentMessage(
		not_null<HistoryItem*> dependent,
		not_null<Data::Story*> dependency) {
	_dependentMessages[dependency].emplace(dependent);
}

void Stories::unregisterDependentMessage(
		not_null<HistoryItem*> dependent,
		not_null<Data::Story*> dependency) {
	const auto i = _dependentMessages.find(dependency);
	if (i != end(_dependentMessages)) {
		if (i->second.remove(dependent) && i->second.empty()) {
			_dependentMessages.erase(i);
		}
	}
}

void Stories::savedStateUpdated(not_null<Story*> story) {
	const auto id = story->id();
	const auto peer = story->peer()->id;
	const auto pinned = story->pinned();
	if (pinned) {
		auto &saved = _saved[peer];
		const auto added = saved.ids.list.emplace(id).second;
		if (added) {
			if (saved.total >= 0 && id > saved.lastId) {
				++saved.total;
			}
			_savedChanged.fire_copy(peer);
		}
	} else if (const auto i = _saved.find(peer); i != end(_saved)) {
		auto &saved = i->second;
		if (saved.ids.list.remove(id)) {
			if (saved.total > 0) {
				--saved.total;
			}
			_savedChanged.fire_copy(peer);
		}
	}
}

void Stories::loadMore(StorySourcesList list) {
	const auto index = static_cast<int>(list);
	if (_loadMoreRequestId[index] || _sourcesLoaded[index]) {
		return;
	}
	const auto hidden = (list == StorySourcesList::Hidden);
	const auto api = &_owner->session().api();
	using Flag = MTPstories_GetAllStories::Flag;
	_loadMoreRequestId[index] = api->request(MTPstories_GetAllStories(
		MTP_flags((hidden ? Flag::f_hidden : Flag())
			| (_sourcesStates[index].isEmpty()
				? Flag(0)
				: (Flag::f_next | Flag::f_state))),
		MTP_string(_sourcesStates[index])
	)).done([=](const MTPstories_AllStories &result) {
		_loadMoreRequestId[index] = 0;

		result.match([&](const MTPDstories_allStories &data) {
			_owner->processUsers(data.vusers());
			_sourcesStates[index] = qs(data.vstate());
			_sourcesLoaded[index] = !data.is_has_more();
			for (const auto &single : data.vuser_stories().v) {
				parseAndApply(single);
			}
		}, [](const MTPDstories_allStoriesNotModified &) {
		});
	}).fail([=] {
		_loadMoreRequestId[index] = 0;
	}).send();
}

void Stories::sendResolveRequests() {
	if (!_resolveSent.empty()) {
		return;
	}
	auto leftToSend = kMaxResolveTogether;
	auto byPeer = base::flat_map<PeerId, QVector<MTPint>>();
	for (auto i = begin(_resolvePending); i != end(_resolvePending);) {
		const auto peerId = i->first;
		auto &ids = i->second;
		auto &sent = _resolveSent[peerId];
		if (ids.size() <= leftToSend) {
			sent = base::take(ids);
			i = _resolvePending.erase(i); // Invalidates `ids`.
			leftToSend -= int(sent.size());
		} else {
			sent = {
				std::make_move_iterator(begin(ids)),
				std::make_move_iterator(begin(ids) + leftToSend)
			};
			ids.erase(begin(ids), begin(ids) + leftToSend);
			leftToSend = 0;
		}
		auto &prepared = byPeer[peerId];
		for (auto &[storyId, callbacks] : sent) {
			prepared.push_back(MTP_int(storyId));
		}
		if (!leftToSend) {
			break;
		}
	}
	const auto api = &_owner->session().api();
	for (auto &entry : byPeer) {
		const auto peerId = entry.first;
		auto &prepared = entry.second;
		const auto finish = [=](PeerId peerId) {
			const auto sent = _resolveSent.take(peerId);
			Assert(sent.has_value());
			for (const auto &[storyId, list] : *sent) {
				finalizeResolve({ peerId, storyId });
				for (const auto &callback : list) {
					callback();
				}
			}
			_itemsChanged.fire_copy(peerId);
			if (_resolveSent.empty() && !_resolvePending.empty()) {
				crl::on_main(&session(), [=] { sendResolveRequests(); });
			}
		};
		const auto user = _owner->session().data().peer(peerId)->asUser();
		if (!user) {
			finish(peerId);
			continue;
		}
		const auto requestId = api->request(MTPstories_GetStoriesByID(
			user->inputUser,
			MTP_vector<MTPint>(prepared)
		)).done([=](const MTPstories_Stories &result) {
			owner().processUsers(result.data().vusers());
			processResolvedStories(user, result.data().vstories().v);
			finish(user->id);
		}).fail([=] {
			finish(peerId);
		}).send();
	 }
}

void Stories::processResolvedStories(
		not_null<PeerData*> peer,
		const QVector<MTPStoryItem> &list) {
	const auto now = base::unixtime::now();
	for (const auto &item : list) {
		item.match([&](const MTPDstoryItem &data) {
			if (!parseAndApply(peer, data, now)) {
				applyDeleted({ peer->id, data.vid().v });
			}
		}, [&](const MTPDstoryItemSkipped &data) {
			LOG(("API Error: Unexpected storyItemSkipped in resolve."));
		}, [&](const MTPDstoryItemDeleted &data) {
			applyDeleted({ peer->id, data.vid().v });
		});
	}
}

void Stories::finalizeResolve(FullStoryId id) {
	const auto already = lookup(id);
	if (!already.has_value() && already.error() == NoStory::Unknown) {
		LOG(("API Error: Could not resolve story %1_%2"
			).arg(id.peer.value
			).arg(id.story));
		applyDeleted(id);
	}
}

void Stories::applyDeleted(FullStoryId id) {
	applyRemovedFromActive(id);

	_deleted.emplace(id);
	const auto i = _stories.find(id.peer);
	if (i != end(_stories)) {
		const auto j = i->second.find(id.story);
		if (j != end(i->second)) {
			// Duplicated in Stories::apply(peer, const MTPUserStories*).
			auto story = std::move(j->second);
			_expiring.remove(story->expires(), story->fullId());
			i->second.erase(j);
			session().changes().storyUpdated(
				story.get(),
				UpdateFlag::Destroyed);
			removeDependencyStory(story.get());
			if (id.peer == session().userPeerId()
				&& _archive.list.remove(id.story)) {
				if (_archiveTotal > 0) {
					--_archiveTotal;
				}
				_archiveChanged.fire({});
			}
			if (story->pinned()) {
				if (const auto k = _saved.find(id.peer); k != end(_saved)) {
					const auto saved = &k->second;
					if (saved->ids.list.remove(id.story)) {
						if (saved->total > 0) {
							--saved->total;
						}
						_savedChanged.fire_copy(id.peer);
					}
				}
			}
			if (_preloading && _preloading->id() == id) {
				preloadFinished(id);
			}
			if (i->second.empty()) {
				_stories.erase(i);
			}
		}
	}
}

void Stories::applyExpired(FullStoryId id) {
	if (const auto maybeStory = lookup(id)) {
		const auto story = *maybeStory;
		if (!story->peer()->isSelf() && !story->pinned()) {
			applyDeleted(id);
			return;
		}
	}
	applyRemovedFromActive(id);
}

void Stories::applyRemovedFromActive(FullStoryId id) {
	const auto removeFromList = [&](StorySourcesList list) {
		const auto index = static_cast<int>(list);
		auto &sources = _sources[index];
		const auto i = ranges::find(
			sources,
			id.peer,
			&StoriesSourceInfo::id);
		if (i != end(sources)) {
			sources.erase(i);
			_sourcesChanged[index].fire({});
		}
	};
	const auto i = _all.find(id.peer);
	if (i != end(_all)) {
		const auto j = i->second.ids.lower_bound(StoryIdDates{ id.story });
		if (j != end(i->second.ids) && j->id == id.story) {
			i->second.ids.erase(j);
			if (i->second.ids.empty()) {
				_all.erase(i);
				removeFromList(StorySourcesList::NotHidden);
				removeFromList(StorySourcesList::Hidden);
			}
			_sourceChanged.fire_copy(id.peer);
		}
	}
}

void Stories::applyDeletedFromSources(PeerId id, StorySourcesList list) {
	auto &sources = _sources[static_cast<int>(list)];
	const auto i = ranges::find(
		sources,
		id,
		&StoriesSourceInfo::id);
	if (i != end(sources)) {
		sources.erase(i);
	}
	_sourcesChanged[static_cast<int>(list)].fire({});
}

void Stories::removeDependencyStory(not_null<Story*> story) {
	const auto i = _dependentMessages.find(story);
	if (i != end(_dependentMessages)) {
		const auto items = std::move(i->second);
		_dependentMessages.erase(i);

		for (const auto &dependent : items) {
			dependent->dependencyStoryRemoved(story);
		}
	}
}

void Stories::sort(StorySourcesList list) {
	const auto index = static_cast<int>(list);
	auto &sources = _sources[index];
	const auto self = _owner->session().userPeerId();
	const auto changelogSenderId = UserData::kServiceNotificationsId;
	const auto proj = [&](const StoriesSourceInfo &info) {
		const auto key = int64(info.last)
			+ (info.premium ? (int64(1) << 47) : 0)
			+ ((info.id == changelogSenderId) ? (int64(1) << 47) : 0)
			+ (info.unread ? (int64(1) << 49) : 0)
			+ ((info.id == self) ? (int64(1) << 50) : 0);
		return std::make_pair(key, info.id);
	};
	ranges::sort(sources, ranges::greater(), proj);
	_sourcesChanged[index].fire({});
	preloadSourcesChanged(list);
}

std::shared_ptr<HistoryItem> Stories::lookupItem(not_null<Story*> story) {
	const auto i = _items.find(story->peer()->id);
	if (i == end(_items)) {
		return nullptr;
	}
	const auto j = i->second.find(story->id());
	if (j == end(i->second)) {
		return nullptr;
	}
	return j->second.lock();
}

std::shared_ptr<HistoryItem> Stories::resolveItem(not_null<Story*> story) {
	auto &items = _items[story->peer()->id];
	auto i = items.find(story->id());
	if (i == end(items)) {
		i = items.emplace(story->id()).first;
	} else if (const auto result = i->second.lock()) {
		return result;
	}
	const auto history = _owner->history(story->peer());
	auto result = std::shared_ptr<HistoryItem>(
		history->makeMessage(story).get(),
		HistoryItem::Destroyer());
	i->second = result;
	return result;
}

std::shared_ptr<HistoryItem> Stories::resolveItem(FullStoryId id) {
	const auto story = lookup(id);
	return story ? resolveItem(*story) : std::shared_ptr<HistoryItem>();
}

const StoriesSource *Stories::source(PeerId id) const {
	const auto i = _all.find(id);
	return (i != end(_all)) ? &i->second : nullptr;
}

const std::vector<StoriesSourceInfo> &Stories::sources(
		StorySourcesList list) const {
	return _sources[static_cast<int>(list)];
}

bool Stories::sourcesLoaded(StorySourcesList list) const {
	return _sourcesLoaded[static_cast<int>(list)];
}

rpl::producer<> Stories::sourcesChanged(StorySourcesList list) const {
	return _sourcesChanged[static_cast<int>(list)].events();
}

rpl::producer<PeerId> Stories::sourceChanged() const {
	return _sourceChanged.events();
}

rpl::producer<PeerId> Stories::itemsChanged() const {
	return _itemsChanged.events();
}

base::expected<not_null<Story*>, NoStory> Stories::lookup(
		FullStoryId id) const {
	const auto i = _stories.find(id.peer);
	if (i != end(_stories)) {
		const auto j = i->second.find(id.story);
		if (j != end(i->second)) {
			return j->second.get();
		}
	}
	return base::make_unexpected(
		_deleted.contains(id) ? NoStory::Deleted : NoStory::Unknown);
}

void Stories::resolve(FullStoryId id, Fn<void()> done) {
	const auto already = lookup(id);
	if (already.has_value() || already.error() != NoStory::Unknown) {
		if (done) {
			done();
		}
		return;
	}
	if (const auto i = _resolveSent.find(id.peer); i != end(_resolveSent)) {
		if (const auto j = i->second.find(id.story); j != end(i->second)) {
			if (done) {
				j->second.push_back(std::move(done));
			}
			return;
		}
	}
	auto &ids = _resolvePending[id.peer];
	if (ids.empty()) {
		crl::on_main(&session(), [=] {
			sendResolveRequests();
		});
	}
	auto &callbacks = ids[id.story];
	if (done) {
		callbacks.push_back(std::move(done));
	}
}

void Stories::loadAround(FullStoryId id, StoriesContext context) {
	if (v::is<StoriesContextSingle>(context.data)) {
		return;
	} else if (v::is<StoriesContextSaved>(context.data)
		|| v::is<StoriesContextArchive>(context.data)) {
		return;
	}
	const auto i = _all.find(id.peer);
	if (i == end(_all)) {
		return;
	}
	const auto j = i->second.ids.lower_bound(StoryIdDates{ id.story });
	if (j == end(i->second.ids) || j->id != id.story) {
		return;
	}
	const auto ignore = [&] {
		const auto side = kIgnorePreloadAroundIfLoaded;
		const auto left = ranges::min(int(j - begin(i->second.ids)), side);
		const auto right = ranges::min(int(end(i->second.ids) - j), side);
		for (auto k = j - left; k != j + right; ++k) {
			const auto maybeStory = lookup({ id.peer, k->id });
			if (!maybeStory && maybeStory.error() == NoStory::Unknown) {
				return false;
			}
		}
		return true;
	}();
	if (ignore) {
		return;
	}
	const auto side = kPreloadAroundCount;
	const auto left = ranges::min(int(j - begin(i->second.ids)), side);
	const auto right = ranges::min(int(end(i->second.ids) - j), side);
	const auto from = j - left;
	const auto till = j + right;
	for (auto k = from; k != till; ++k) {
		resolve({ id.peer, k->id }, nullptr);
	}
}

void Stories::markAsRead(FullStoryId id, bool viewed) {
	if (id.peer == _owner->session().userPeerId()) {
		return;
	}
	const auto maybeStory = lookup(id);
	if (!maybeStory) {
		return;
	}
	const auto story = *maybeStory;
	if (story->expired() && story->pinned()) {
		_incrementViewsPending[id.peer].emplace(id.story);
		if (!_incrementViewsTimer.isActive()) {
			_incrementViewsTimer.callOnce(kIncrementViewsDelay);
		}
	}
	const auto i = _all.find(id.peer);
	if (i == end(_all) || i->second.readTill >= id.story) {
		return;
	} else if (!_markReadPending.contains(id.peer)) {
		sendMarkAsReadRequests();
	}
	_markReadPending.emplace(id.peer);
	const auto wasUnread = i->second.unread();
	i->second.readTill = id.story;
	const auto nowUnread = i->second.unread();
	if (wasUnread != nowUnread) {
		const auto refreshInList = [&](StorySourcesList list) {
			auto &sources = _sources[static_cast<int>(list)];
			const auto i = ranges::find(
				sources,
				id.peer,
				&StoriesSourceInfo::id);
			if (i != end(sources)) {
				i->unread = nowUnread;
				sort(list);
			}
		};
		refreshInList(StorySourcesList::NotHidden);
		refreshInList(StorySourcesList::Hidden);
	}
	_markReadTimer.callOnce(kMarkAsReadDelay);
}

void Stories::toggleHidden(
		PeerId peerId,
		bool hidden,
		std::shared_ptr<Ui::Show> show) {
	const auto user = _owner->peer(peerId)->asUser();
	Assert(user != nullptr);
	if (user->hasStoriesHidden() != hidden) {
		user->setFlags(hidden
			? (user->flags() | UserDataFlag::StoriesHidden)
			: (user->flags() & ~UserDataFlag::StoriesHidden));
		session().api().request(MTPcontacts_ToggleStoriesHidden(
			user->inputUser,
			MTP_bool(hidden)
		)).send();
	}

	const auto name = user->shortName();
	const auto guard = gsl::finally([&] {
		if (show) {
			const auto phrase = hidden
				? tr::lng_stories_hidden_to_contacts
				: tr::lng_stories_shown_in_chats;
			show->showToast(phrase(
				tr::now,
				lt_user,
				Ui::Text::Bold(name),
				Ui::Text::WithEntities));
		}
	});

	const auto i = _all.find(peerId);
	if (i == end(_all)) {
		return;
	}
	i->second.hidden = hidden;
	const auto info = i->second.info();
	const auto main = static_cast<int>(StorySourcesList::NotHidden);
	const auto other = static_cast<int>(StorySourcesList::Hidden);
	const auto proj = &StoriesSourceInfo::id;
	if (hidden) {
		const auto i = ranges::find(_sources[main], peerId, proj);
		if (i != end(_sources[main])) {
			_sources[main].erase(i);
			_sourcesChanged[main].fire({});
			preloadSourcesChanged(StorySourcesList::NotHidden);
		}
		const auto j = ranges::find(_sources[other], peerId, proj);
		if (j == end(_sources[other])) {
			_sources[other].push_back(info);
		} else {
			*j = info;
		}
		sort(StorySourcesList::Hidden);
	} else {
		const auto i = ranges::find(_sources[other], peerId, proj);
		if (i != end(_sources[other])) {
			_sources[other].erase(i);
			_sourcesChanged[other].fire({});
			preloadSourcesChanged(StorySourcesList::Hidden);
		}
		const auto j = ranges::find(_sources[main], peerId, proj);
		if (j == end(_sources[main])) {
			_sources[main].push_back(info);
		} else {
			*j = info;
		}
		sort(StorySourcesList::NotHidden);
	}
}

void Stories::sendMarkAsReadRequest(
		not_null<PeerData*> peer,
		StoryId tillId) {
	Expects(peer->isUser());

	const auto peerId = peer->id;
	_markReadRequests.emplace(peerId);
	const auto finish = [=] {
		_markReadRequests.remove(peerId);
		if (!_markReadTimer.isActive()
			&& _markReadPending.contains(peerId)) {
			sendMarkAsReadRequests();
		}
		checkQuitPreventFinished();
	};

	const auto api = &_owner->session().api();
	api->request(MTPstories_ReadStories(
		peer->asUser()->inputUser,
		MTP_int(tillId)
	)).done(finish).fail(finish).send();
}

void Stories::checkQuitPreventFinished() {
	if (_markReadRequests.empty() && _incrementViewsRequests.empty()) {
		if (Core::Quitting()) {
			LOG(("Stories doesn't prevent quit any more."));
		}
		Core::App().quitPreventFinished();
	}
}

void Stories::sendMarkAsReadRequests() {
	_markReadTimer.cancel();
	for (auto i = begin(_markReadPending); i != end(_markReadPending);) {
		const auto peerId = *i;
		if (_markReadRequests.contains(peerId)) {
			++i;
			continue;
		}
		const auto j = _all.find(peerId);
		if (j != end(_all)) {
			sendMarkAsReadRequest(j->second.user, j->second.readTill);
		}
		i = _markReadPending.erase(i);
	}
}

void Stories::sendIncrementViewsRequests() {
	if (_incrementViewsPending.empty()) {
		return;
	}
	auto ids = QVector<MTPint>();
	auto peer = PeerId();
	struct Prepared {
		PeerId peer = 0;
		QVector<MTPint> ids;
	};
	auto prepared = std::vector<Prepared>();
	for (const auto &[peer, ids] : _incrementViewsPending) {
		if (_incrementViewsRequests.contains(peer)) {
			continue;
		}
		prepared.push_back({ .peer = peer });
		for (const auto &id : ids) {
			prepared.back().ids.push_back(MTP_int(id));
		}
	}

	const auto api = &_owner->session().api();
	for (auto &[peer, ids] : prepared) {
		_incrementViewsRequests.emplace(peer);
		const auto finish = [=, peer = peer] {
			_incrementViewsRequests.remove(peer);
			if (!_incrementViewsTimer.isActive()
				&& _incrementViewsPending.contains(peer)) {
				sendIncrementViewsRequests();
			}
			checkQuitPreventFinished();
		};
		api->request(MTPstories_IncrementStoryViews(
			_owner->peer(peer)->asUser()->inputUser,
			MTP_vector<MTPint>(std::move(ids))
		)).done(finish).fail(finish).send();
		_incrementViewsPending.remove(peer);
	}
}

void Stories::loadViewsSlice(
		StoryId id,
		std::optional<StoryView> offset,
		Fn<void(std::vector<StoryView>)> done) {
	_viewsDone = std::move(done);
	if (_viewsStoryId == id && _viewsOffset == offset) {
		return;
	}
	_viewsStoryId = id;
	_viewsOffset = offset;

	const auto api = &_owner->session().api();
	api->request(_viewsRequestId).cancel();
	_viewsRequestId = api->request(MTPstories_GetStoryViewsList(
		MTP_int(id),
		MTP_int(offset ? offset->date : 0),
		MTP_long(offset ? peerToUser(offset->peer->id).bare : 0),
		MTP_int(kViewsPerPage)
	)).done([=](const MTPstories_StoryViewsList &result) {
		_viewsRequestId = 0;

		auto slice = std::vector<StoryView>();

		const auto &data = result.data();
		_owner->processUsers(data.vusers());
		slice.reserve(data.vviews().v.size());
		for (const auto &view : data.vviews().v) {
			slice.push_back({
				.peer = _owner->peer(peerFromUser(view.data().vuser_id())),
				.date = view.data().vdate().v,
			});
		}
		const auto fullId = FullStoryId{
			.peer = _owner->session().userPeerId(),
			.story = _viewsStoryId,
		};
		if (const auto story = lookup(fullId)) {
			(*story)->applyViewsSlice(_viewsOffset, slice, data.vcount().v);
		}
		if (const auto done = base::take(_viewsDone)) {
			done(std::move(slice));
		}
	}).fail([=] {
		_viewsRequestId = 0;
		if (const auto done = base::take(_viewsDone)) {
			done({});
		}
	}).send();
}

const StoriesIds &Stories::archive() const {
	return _archive;
}

rpl::producer<> Stories::archiveChanged() const {
	return _archiveChanged.events();
}

int Stories::archiveCount() const {
	return std::max(_archiveTotal, 0);
}

bool Stories::archiveCountKnown() const {
	return _archiveTotal >= 0;
}

bool Stories::archiveLoaded() const {
	return _archiveLoaded;
}

const StoriesIds *Stories::saved(PeerId peerId) const {
	const auto i = _saved.find(peerId);
	return (i != end(_saved)) ? &i->second.ids : nullptr;
}

rpl::producer<PeerId> Stories::savedChanged() const {
	return _savedChanged.events();
}

int Stories::savedCount(PeerId peerId) const {
	const auto i = _saved.find(peerId);
	return (i != end(_saved)) ? i->second.total : 0;
}

bool Stories::savedCountKnown(PeerId peerId) const {
	const auto i = _saved.find(peerId);
	return (i != end(_saved)) && (i->second.total >= 0);
}

bool Stories::savedLoaded(PeerId peerId) const {
	const auto i = _saved.find(peerId);
	return (i != end(_saved)) && i->second.loaded;
}

void Stories::archiveLoadMore() {
	if (_archiveRequestId || _archiveLoaded) {
		return;
	}
	const auto api = &_owner->session().api();
	_archiveRequestId = api->request(MTPstories_GetStoriesArchive(
		MTP_int(_archiveLastId),
		MTP_int(_archiveLastId ? kArchivePerPage : kArchiveFirstPerPage)
	)).done([=](const MTPstories_Stories &result) {
		_archiveRequestId = 0;

		const auto &data = result.data();
		const auto self = _owner->session().user();
		const auto now = base::unixtime::now();
		_archiveTotal = data.vcount().v;
		for (const auto &story : data.vstories().v) {
			const auto id = story.match([&](const auto &id) {
				return id.vid().v;
			});
			_archive.list.emplace(id);
			_archiveLastId = id;
			if (!parseAndApply(self, story, now)) {
				_archive.list.remove(id);
				if (_archiveTotal > 0) {
					--_archiveTotal;
				}
			}
		}
		const auto ids = int(_archive.list.size());
		_archiveLoaded = data.vstories().v.empty();
		_archiveTotal = _archiveLoaded ? ids : std::max(_archiveTotal, ids);
		_archiveChanged.fire({});
	}).fail([=] {
		_archiveRequestId = 0;
		_archiveLoaded = true;
		_archiveTotal = int(_archive.list.size());
		_archiveChanged.fire({});
	}).send();
}

void Stories::savedLoadMore(PeerId peerId) {
	Expects(peerIsUser(peerId));

	auto &saved = _saved[peerId];
	if (saved.requestId || saved.loaded) {
		return;
	}
	const auto api = &_owner->session().api();
	const auto peer = _owner->peer(peerId);
	saved.requestId = api->request(MTPstories_GetPinnedStories(
		peer->asUser()->inputUser,
		MTP_int(saved.lastId),
		MTP_int(saved.lastId ? kSavedPerPage : kSavedFirstPerPage)
	)).done([=](const MTPstories_Stories &result) {
		auto &saved = _saved[peerId];
		saved.requestId = 0;

		const auto &data = result.data();
		const auto now = base::unixtime::now();
		saved.total = data.vcount().v;
		for (const auto &story : data.vstories().v) {
			const auto id = story.match([&](const auto &id) {
				return id.vid().v;
			});
			saved.ids.list.emplace(id);
			saved.lastId = id;
			if (!parseAndApply(peer, story, now)) {
				saved.ids.list.remove(id);
				if (saved.total > 0) {
					--saved.total;
				}
			}
		}
		const auto ids = int(saved.ids.list.size());
		saved.loaded = data.vstories().v.empty();
		saved.total = saved.loaded ? ids : std::max(saved.total, ids);
		_savedChanged.fire_copy(peerId);
	}).fail([=] {
		auto &saved = _saved[peerId];
		saved.requestId = 0;
		saved.loaded = true;
		saved.total = int(saved.ids.list.size());
		_savedChanged.fire_copy(peerId);
	}).send();
}

void Stories::deleteList(const std::vector<FullStoryId> &ids) {
	auto list = QVector<MTPint>();
	list.reserve(ids.size());
	const auto selfId = session().userPeerId();
	for (const auto &id : ids) {
		if (id.peer == selfId) {
			list.push_back(MTP_int(id.story));
		}
	}
	if (!list.empty()) {
		const auto api = &_owner->session().api();
		api->request(MTPstories_DeleteStories(
			MTP_vector<MTPint>(list)
		)).done([=](const MTPVector<MTPint> &result) {
			for (const auto &id : result.v) {
				applyDeleted({ selfId, id.v });
			}
		}).send();
	}
}

void Stories::report(
		std::shared_ptr<Ui::Show> show,
		FullStoryId id,
		Ui::ReportReason reason,
		QString text) {
	if (const auto maybeStory = lookup(id)) {
		const auto story = *maybeStory;
		Api::SendReport(show, story->peer(), reason, text, story->id());
	}
}

bool Stories::isQuitPrevent() {
	if (!_markReadPending.empty()) {
		sendMarkAsReadRequests();
	}
	if (!_incrementViewsPending.empty()) {
		sendIncrementViewsRequests();
	}
	if (_markReadRequests.empty() && _incrementViewsRequests.empty()) {
		return false;
	}
	LOG(("Stories prevents quit, marking as read..."));
	return true;
}

void Stories::incrementPreloadingMainSources() {
	Expects(_preloadingMainSourcesCounter >= 0);

	if (++_preloadingMainSourcesCounter == 1
		&& rebuildPreloadSources(StorySourcesList::NotHidden)) {
		continuePreloading();
	}
}

void Stories::decrementPreloadingMainSources() {
	Expects(_preloadingMainSourcesCounter > 0);

	if (!--_preloadingMainSourcesCounter
		&& rebuildPreloadSources(StorySourcesList::NotHidden)) {
		continuePreloading();
	}
}

void Stories::incrementPreloadingHiddenSources() {
	Expects(_preloadingHiddenSourcesCounter >= 0);

	if (++_preloadingHiddenSourcesCounter == 1
		&& rebuildPreloadSources(StorySourcesList::Hidden)) {
		continuePreloading();
	}
}

void Stories::decrementPreloadingHiddenSources() {
	Expects(_preloadingHiddenSourcesCounter > 0);

	if (!--_preloadingHiddenSourcesCounter
		&& rebuildPreloadSources(StorySourcesList::Hidden)) {
		continuePreloading();
	}
}

void Stories::setPreloadingInViewer(std::vector<FullStoryId> ids) {
	ids.erase(ranges::remove_if(ids, [&](FullStoryId id) {
		return _preloaded.contains(id);
	}), end(ids));
	if (_toPreloadViewer != ids) {
		_toPreloadViewer = std::move(ids);
		continuePreloading();
	}
}

void Stories::preloadSourcesChanged(StorySourcesList list) {
	if (rebuildPreloadSources(list)) {
		continuePreloading();
	}
}

bool Stories::rebuildPreloadSources(StorySourcesList list) {
	const auto index = static_cast<int>(list);
	const auto &counter = (list == StorySourcesList::Hidden)
		? _preloadingHiddenSourcesCounter
		: _preloadingMainSourcesCounter;
	if (!counter) {
		return !base::take(_toPreloadSources[index]).empty();
	}
	auto now = std::vector<FullStoryId>();
	auto processed = 0;
	for (const auto &source : _sources[index]) {
		const auto i = _all.find(source.id);
		if (i != end(_all)) {
			if (const auto id = i->second.toOpen().id) {
				const auto fullId = FullStoryId{ source.id, id };
				if (!_preloaded.contains(fullId)) {
					now.push_back(fullId);
				}
			}
		}
		if (++processed >= kMaxPreloadSources) {
			break;
		}
	}
	if (now != _toPreloadSources[index]) {
		_toPreloadSources[index] = std::move(now);
		return true;
	}
	return false;
}

void Stories::continuePreloading() {
	const auto now = _preloading ? _preloading->id() : FullStoryId();
	if (now) {
		if (shouldContinuePreload(now)) {
			return;
		}
		_preloading = nullptr;
	}
	const auto id = nextPreloadId();
	if (!id) {
		return;
	} else if (const auto maybeStory = lookup(id)) {
		startPreloading(*maybeStory);
	}
}

bool Stories::shouldContinuePreload(FullStoryId id) const {
	const auto first = ranges::views::concat(
		_toPreloadViewer,
		_toPreloadSources[static_cast<int>(StorySourcesList::Hidden)],
		_toPreloadSources[static_cast<int>(StorySourcesList::NotHidden)]
	) | ranges::views::take(kStillPreloadFromFirst);
	return ranges::contains(first, id);
}

FullStoryId Stories::nextPreloadId() const {
	const auto hidden = static_cast<int>(StorySourcesList::Hidden);
	const auto main = static_cast<int>(StorySourcesList::NotHidden);
	const auto result = !_toPreloadViewer.empty()
		? _toPreloadViewer.front()
		: !_toPreloadSources[hidden].empty()
		? _toPreloadSources[hidden].front()
		: !_toPreloadSources[main].empty()
		? _toPreloadSources[main].front()
		: FullStoryId();

	Ensures(!_preloaded.contains(result));
	return result;
}

void Stories::startPreloading(not_null<Story*> story) {
	Expects(!_preloaded.contains(story->fullId()));

	const auto id = story->fullId();
	auto preloading = std::make_unique<StoryPreload>(story, [=] {
		_preloading = nullptr;
		preloadFinished(id, true);
	});
	if (!_preloaded.contains(id)) {
		_preloading = std::move(preloading);
	}
}

void Stories::preloadFinished(FullStoryId id, bool markAsPreloaded) {
	for (auto &sources : _toPreloadSources) {
		sources.erase(ranges::remove(sources, id), end(sources));
	}
	_toPreloadViewer.erase(
		ranges::remove(_toPreloadViewer, id),
		end(_toPreloadViewer));
	if (markAsPreloaded) {
		_preloaded.emplace(id);
	}
	crl::on_main(this, [=] {
		continuePreloading();
	});
}

} // namespace Data