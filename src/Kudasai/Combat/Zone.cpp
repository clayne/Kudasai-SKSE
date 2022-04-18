#include "Kudasai/Combat/Zone.h"

#include "Kudasai/Combat/Resolution.h"
#include "Kudasai/Defeat.h"
#include "Kudasai/Struggle/Struggly.h"
#include "Papyrus/Settings.h"

namespace Kudasai
{
	namespace Config = Papyrus::Configuration;

	bool Zone::registerdefeat(RE::Actor* victim, RE::Actor* aggressor)
	{
		logger::info("{} -> Register Defeat with Aggressor = {}", victim->GetFormID(), aggressor->GetFormID());
		if (aggressor->IsPlayerRef()) {
			std::thread(&Zone::defeat, victim, aggressor, DefeatResult::Defeat).detach();
			return true;
		}

		auto agrzone = aggressor->GetCombatGroup();
		// auto viczone = victim->GetCombatGroup();
		if (!agrzone) {
			logger::warn("{} -> Failed to register defeat, aggressors Combat Group is missing", victim->GetFormID());
			return false;
		}
		// victim cant be commanded, hit evaluation already checks that
		if (aggressor->IsCommandedActor()) {
			auto tmp = aggressor->GetCommandingActor().get();
			if (tmp) {
				logger::info("{} -> Aggressor is summon, using commander as aggressor = {}", victim->GetFormID(), tmp->GetFormID());
				aggressor = tmp.get();
			} else {
				logger::warn("{} -> Aggressor is commanded but no commander found? Abandon", victim->GetFormID());
				return false;
			}
		}
		auto dtype = getdefeattype(agrzone);
		if (dtype == DefeatResult::Cancel)
			return false;
		std::thread(&Zone::defeat, victim, aggressor, dtype).detach();
		return true;
	}

	Zone::DefeatResult Zone::getdefeattype(RE::CombatGroup* agrzone)
	{
		using Res = Zone::DefeatResult;
		const auto tarnum = countvalid(agrzone->targets);
		if (countvalid(agrzone->members) == 0 || tarnum == 0)
			return Res::Cancel;
		return tarnum == 1 ? Res::Resolution : Res::Defeat;
	}

	void Zone::defeat(RE::Actor* victim, RE::Actor* aggressor, DefeatResult result)
	{
		// delay to make the player be defeated 'after' the hit
		std::this_thread::sleep_for(std::chrono::microseconds(450));

		static std::mutex _m;
		std::scoped_lock lock(_m);
		if (victim->IsDead() || victim->IsInKillMove() || Defeat::isdefeated(victim)) {
			logger::info("{} -> Victim is dead, defeated or in killmove", victim->GetFormID());
			if (result == DefeatResult::Resolution)
				victim = nullptr;
			else
				return;
		} else {
			const auto process = victim->currentProcess;
			const auto middlehigh = process ? process->middleHigh : nullptr;
			if (middlehigh) {
				for (auto& data : middlehigh->commandedActors) {
					const auto eff = data.activeEffect;
					if (eff)
						victim->InvalidateCommandedActorEffect(eff);
				}
			}
			if (!victim->IsPlayerRef()) {
				if (Papyrus::GetSetting<bool>("bNotifyDefeat")) {
					std::string msg;
					if (Papyrus::GetSetting<bool>("bNotifyColored")) {
						auto color = Papyrus::GetSetting<RE::BSFixedString>("sNotifyColorChoice");
						msg = fmt::format("<font color = '{}'>{} has been defeated by {}</font color>", color, victim->GetDisplayFullName(), aggressor->GetDisplayFullName());
					} else {
						msg = fmt::format("{} has been defeated by {}", victim->GetDisplayFullName(), aggressor->GetDisplayFullName());
					}
					RE::DebugNotification(msg.c_str());
				}
			}
		}
		const auto getmemberlist = [&aggressor]() -> std::vector<RE::Actor*> {
			if (const auto cg = aggressor->GetCombatGroup(); cg) {
				std::vector<RE::Actor*> ret{};
				for (auto& e : cg->members)
					if (const auto actor = e.memberHandle.get(); actor && !actor->IsDead() && !Defeat::isdefeated(actor.get()))
						ret.push_back(actor.get());
				return ret;
			} else {
				return { aggressor };
			}
		};

		switch (result) {
		case DefeatResult::Resolution:
			if (victim)
				Defeat::defeat(victim);

			if (const auto player = RE::PlayerCharacter::GetSingleton(); Serialize::GetSingleton()->Defeated.contains(0x14) && aggressor->IsHostileToActor(player)) {
				PlayerDefeat::Unregister();
				if (aggressor->IsPlayerTeammate()) {
					if (const auto q = Resolution::GetQuestFriendly(getmemberlist()); q->Start())
						return;
				} else if (aggressor->IsHostileToActor(player)) {
					auto q = Resolution::GetQuestHostile(getmemberlist(), false);
					if (!q)	 // Fallback Quest "ToMapEdge"
						q = RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESQuest>(0x88C931, ESPNAME);
					if (q->Start())
						return;
				}
				std::this_thread::sleep_for(std::chrono::seconds(6));
				Defeat::rescue(player, false);
				std::this_thread::sleep_for(std::chrono::seconds(3));
				Defeat::undopacify(player);
			} else if (!aggressor->IsPlayerTeammate() && Papyrus::GetSetting<bool>("bPostCombatAssault")) {	 // followers do not start the resolution quest
				CreateNPCResolution(aggressor);
			}
			break;
		case DefeatResult::Defeat:
			if (victim->IsPlayerRef()) {
				if (randomREAL<float>(0, 99.5) < Papyrus::GetSetting<float>("fMidCombatBlackout")) {
					if (const auto q = Resolution::GetQuestHostile(getmemberlist(), false); q->Start())
						return;
				} else {
					PlayerDefeat::Register();
				}
			}
			Defeat::defeat(victim);
			break;
		}
	}

	void Zone::CreateNPCResolution(RE::Actor* aggressor)
	{
		const auto npcresQ = RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESQuest>(0x8130AF, ESPNAME);
		if (!npcresQ->IsStopped()) {
			logger::warn("NPC Resolution already running");
			return;
		}
		const auto cg = aggressor->GetCombatGroup();
		if (!cg)
			return;

		std::vector<std::pair<RE::Actor*, std::vector<RE::Actor*>>> viclist;
		{
			std::set<RE::Actor*> agrlist;
			for (auto& e : cg->members) {  // populate lists
				auto ptr = e.memberHandle.get();
				if (ptr && Struggle::FindPair(ptr.get()) == nullptr)
					agrlist.insert(ptr.get());
			}
			auto& Defeated = Serialize::GetSingleton()->Defeated;
			for (auto it = Defeated.begin(); it != Defeated.end();) {
				auto actor = RE::TESForm::LookupByID<RE::Actor>(*it);
				if (!actor || actor->IsDead())
					it = Defeated.erase(it);
				else {
					if (actor->Is3DLoaded() && Config::IsValidRace(actor) && Struggle::FindPair(actor) == nullptr) {
						if (!actor->IsHostileToActor(aggressor))
							agrlist.insert(actor);
						else if (viclist.size() < 15)
							viclist.push_back(std::make_pair(actor, std::vector<RE::Actor*>()));
					}
					it++;
				}
			}
			if (viclist.empty())
				return;

			const auto GetDistance = [](RE::Actor* prim, RE::Actor* sec) -> float {
				return prim->GetPosition().GetDistance(sec->GetPosition());
			};
			// assign every aggressor to a victim closest to them
			for (auto& victoire : agrlist) {
				if (randomINT<short>(0, 99) < 10) {	 // oddity an aggressor will ignore the scene
					continue;
				}
				for (auto& [defeated, victoires] : viclist) {
					const auto distance = GetDistance(defeated, victoire);
					if (distance < 1500.0f && Config::IsInterested(defeated, victoire)) {  // interested?
						for (auto& [key, value] : viclist) {							   // already in a previous list?
							if (key == defeated) {										   // no previous list
								victoires.push_back(victoire);
								break;
							}
							auto where = std::find(value.begin(), value.end(), victoire);
							if (where != value.end()) {	 // in previous list
								// assume victoire will switch targets only if new target is closer + 30% chance
								if (distance < GetDistance(key, victoire) && randomINT<short>(0, 99) < 30) {
									value.erase(where);
									value.push_back(victoire);
								}
								break;	// victoire can only be in at most one previous vector
							}
						}
					}
				}
			}
		}
		// at this point, viclist is a map with <= 15 elements, mapping victims to a vector of aggressors interested in it
		// the quest holds at most 50 victoires. Remove any lone victims and any group that would break the 50 Actor limit
		size_t total = 0;
		for (auto entry = viclist.begin(); entry != viclist.end();) {
			if (entry->second.empty()) {
				entry = viclist.erase(entry);
			} else {
				if (total += entry->second.size(); total >= 50) {
					viclist.erase(entry, viclist.end());
					break;
				}
				entry++;
			}
		}
		if (viclist.empty())
			return;

		auto task = SKSE::GetTaskInterface();
		task->AddTask([npcresQ, viclist]() {
			const auto handler = RE::TESDataHandler::GetSingleton();
			const std::vector<RE::BGSKeyword*> links{
				handler->LookupForm<RE::BGSKeyword>(0x81309F, ESPNAME),
				handler->LookupForm<RE::BGSKeyword>(0x80DF8D, ESPNAME),
				handler->LookupForm<RE::BGSKeyword>(0x813093, ESPNAME),
				handler->LookupForm<RE::BGSKeyword>(0x813094, ESPNAME),
				handler->LookupForm<RE::BGSKeyword>(0x813095, ESPNAME),
				handler->LookupForm<RE::BGSKeyword>(0x813096, ESPNAME),
				handler->LookupForm<RE::BGSKeyword>(0x813097, ESPNAME),
				handler->LookupForm<RE::BGSKeyword>(0x813098, ESPNAME),
				handler->LookupForm<RE::BGSKeyword>(0x813099, ESPNAME),
				handler->LookupForm<RE::BGSKeyword>(0x81309A, ESPNAME),
				handler->LookupForm<RE::BGSKeyword>(0x81309B, ESPNAME),
				handler->LookupForm<RE::BGSKeyword>(0x81309C, ESPNAME),
				handler->LookupForm<RE::BGSKeyword>(0x81309D, ESPNAME),
				handler->LookupForm<RE::BGSKeyword>(0x81309E, ESPNAME),
				handler->LookupForm<RE::BGSKeyword>(0x8130A0, ESPNAME)
			};
			const auto tmpfriends = handler->LookupForm<RE::TESFaction>(0x7C714B, ESPNAME);

			int i = 0;
			for (auto& [victim, list] : viclist) {
				for (auto& victoire : list)
					victoire->extraList.SetLinkedRef(victim, links[i]);
				victim->AddToFaction(tmpfriends, 0);
				i++;
			}

			if (!npcresQ->Start()) {
				logger::warn("Failed to start NPC Resolution");
				int n = 0;
				for (auto& [victim, list] : viclist) {
					for (auto& victoire : list)
						victoire->extraList.SetLinkedRef(nullptr, links[n]);
					RemoveFromFaction(victim, tmpfriends);
					n++;
				}
			}
		});
	}

	void PlayerDefeat::Register()
	{
		auto me = GetSingleton();
		me->Active = true;
		std::thread(&Cycle, me).detach();
	}

	void PlayerDefeat::Unregister()
	{
		GetSingleton()->Active = false;
	}

	void PlayerDefeat::Cycle()
	{
		const auto main = RE::Main::GetSingleton();
		const auto player = RE::PlayerCharacter::GetSingleton();
		const auto playerxyz = player->GetPosition();
		const auto processLists = RE::ProcessLists::GetSingleton();

		do {
			std::this_thread::sleep_for(std::chrono::seconds(4));
			if (!main->gameActive)
				goto Conntinue;

			if (!Defeat::isdefeated(player))
				return;

			for (auto& e : processLists->highActorHandles) {
				auto ptr = e.get();
				if (ptr == nullptr)
					continue;

				if (ptr->IsHostileToActor(player) && ptr->GetPosition().GetDistance(playerxyz) < 4096.0f) {
					if (ptr->IsInCombat())
						goto Conntinue;
					else if (auto q = Resolution::GetQuestHostile(std::vector{ ptr.get() }, false); q && q->Start())
						return;
				}
			}
			Defeat::rescue(player, true);
			return;

Conntinue:;
		} while (Active);
	}

}  // namespace Kudasai
