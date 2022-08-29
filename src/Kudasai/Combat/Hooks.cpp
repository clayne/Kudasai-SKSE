#include "Kudasai/Combat/Hooks.h"

#include "Kudasai/Combat/Zone.h"
#include "Kudasai/Defeat.h"
#include "Papyrus/Settings.h"
#include "Kudasai/EventSink.h"

namespace Config = Papyrus::Configuration;
using Archetype = RE::EffectArchetypes::ArchetypeID;

namespace Kudasai
{
	void Hooks::InstallHook()
	{
		SKSE::AllocTrampoline(1 << 7);
		auto& trampoline = SKSE::GetTrampoline();
		// ==================================================
		REL::Relocation<std::uintptr_t> wh{ RELID(37673, 38627) };
		_WeaponHit = trampoline.write_call<5>(wh.address() + OFFSET(0x3C0, 0x4a8), WeaponHit);
		// ==================================================
		REL::Relocation<std::uintptr_t> t{ RELID(33763, 34547) };
		_MagicHit = trampoline.write_call<5>(t.address() + OFFSET(0x52F, 0x7B1), MagicHit);
		// ==================================================
#ifndef SKYRIM_SUPPORT_AE
		REL::Relocation<std::uintptr_t> ma{ RELID(37832, 39865) };	// 38786
		_IsMagicImmune = trampoline.write_call<5>(ma.address() + OFFSET(0x3B, 0x3F), IsMagicImmune);
#endif
		// ==================================================
		REL::Relocation<std::uintptr_t> det{ RELID(41659, 42742) };
		_DoDetect = trampoline.write_call<5>(det.address() + OFFSET(0x526, 0x67B), DoDetect);
		// ==================================================
		REL::Relocation<std::uintptr_t> explH{ RELID(42677, 43849) };
		_ExplosionHit = trampoline.write_call<5>(explH.address() + OFFSET(0x38C, 0x3C2), ExplosionHit);
		// ==================================================
		REL::Relocation<std::uintptr_t> plu{ RE::PlayerCharacter::VTABLE[0] };
		_PlUpdate = plu.write_vfunc(0xAD, PlUpdate);

		logger::info("Hooks installed");
	}  // InstallHook()

	inline void Hooks::PlUpdate(RE::PlayerCharacter* player, float delta)
	{
		_PlUpdate(player, delta);
		const auto checkdot = [](RE::Actor* victim) -> void {
			const auto& effects = victim->GetActiveEffectList();
			if (!effects)
				return;
			float total = 0.0f;
			for (const auto& effect : *effects) {
				if (!effect || effect->flags.any(RE::ActiveEffect::Flag::kDispelled, RE::ActiveEffect::Flag::kInactive))
					continue;
				else if (const float change = GetExpectedHealthModification(effect); change != 0)
					total += change / 20;  // Only consider damage the spell would do within the next 50ms
			}
			if (total < 0 && victim->GetActorValue(RE::ActorValue::kHealth) <= fabs(total)) {
				if (auto aggressor = GetNearValidAggressor(victim); aggressor) {
					if (GetDefeated(victim, aggressor, true) != HitResult::Proceed) {
						if (Kudasai::Zone::registerdefeat(victim, aggressor)) {
							Defeat::SetDamageImmune(victim);
							RemoveDamagingSpells(victim);
						}
					}
				}
			}
		};

		if (const auto settings = Papyrus::Settings::GetSingleton(); settings->bEnabled && settings->AllowProcessing && Papyrus::Configuration::IsValidPrerequisite()) {
			checkdot(player);

			const auto processLists = RE::ProcessLists::GetSingleton();
			for (auto& handle : processLists->highActorHandles) {
				auto subject = handle.get();
				if (!subject || subject->IsDead() || Defeat::IsDamageImmune(subject.get()) || !Papyrus::Configuration::IsNPC(subject.get()) || subject->IsCommandedActor())
					continue;
				checkdot(subject.get());
			}
		}
	}

	void Hooks::WeaponHit(RE::Actor* a_target, RE::HitData& a_hitData)
	{
		const auto aggressor = a_hitData.aggressor.get();
		if (a_target && aggressor && aggressor.get() != a_target && !a_target->IsCommandedActor() && Config::IsNPC(a_target)) {
			if (Defeat::IsDamageImmune(a_target))
				return;
			const auto settings = Papyrus::Settings::GetSingleton();
			if (settings->bEnabled && settings->AllowProcessing && Papyrus::Configuration::IsValidPrerequisite()) {
				const float hp = a_target->GetActorValue(RE::ActorValue::kHealth);
				auto dmg = a_hitData.totalDamage + fabs(GetIncomingEffectDamage(a_target));
				AdjustByDifficultyMult(dmg, aggressor->IsPlayerRef());
				const auto t = GetDefeated(a_target, aggressor.get(), hp <= dmg);
				if (t != HitResult::Proceed && Kudasai::Zone::registerdefeat(a_target, aggressor.get())) {
					Defeat::SetDamageImmune(a_target);
					RemoveDamagingSpells(a_target);
					if (t == HitResult::Lethal) {
						if (hp < 2)
							return;
						dmg = hp - 0.05f;
					}
					a_target->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -dmg);
					return;
				} else if ((a_hitData.flags.underlying() & ((1 << 0) + (1 << 1))) == 0) {  // blocked, blocked with weapon
					ValidateStrip(a_target);
				}
			}
		}
		return _WeaponHit(a_target, a_hitData);
	}  // WeaponHit()

	void Hooks::MagicHit(uint64_t* unk1, RE::ActiveEffect& effect, uint64_t* unk3, uint64_t* unk4, uint64_t* unk5)
	{
		const auto target = effect.GetTargetActor();
		const auto& base = effect.effect ? effect.effect->baseEffect : nullptr;
		if (!target || !base || target->IsCommandedActor() || !Papyrus::Configuration::IsNPC(target))
			return _MagicHit(unk1, effect, unk3, unk4, unk5);
		enum
		{
			damaging,
			healing,
			none
		};
		switch ([&]() {
			if (effect.magnitude != 0 && (base->data.primaryAV == RE::ActorValue::kHealth || base->data.secondaryAV == RE::ActorValue::kHealth))
				return effect.magnitude < 0 ? damaging : healing;
			return none;
		}()) {
		case healing:
			if (Defeat::isdefeated(target))
				Defeat::rescue(target, true);
			break;
		case damaging:
			if (Defeat::isdefeated(target))
				return;
			if (const auto settings = Papyrus::Settings::GetSingleton(); settings->bEnabled && settings->AllowProcessing && Papyrus ::Configuration::IsValidPrerequisite()) {
				const auto caster = [&]() -> RE::Actor* {
					if (const auto caster = effect.caster.get(); caster)
						return caster.get();
					return nullptr;
				}();
				if (caster && caster != target) {
					const float health = target->GetActorValue(RE::ActorValue::kHealth);
					float dmg = base->data.secondaryAV == RE::ActorValue::kHealth ? effect.magnitude * base->data.secondAVWeight : effect.magnitude;
					dmg += GetIncomingEffectDamage(target);	 // + GetTaperDamage(effect.magnitude, data->data);
					AdjustByDifficultyMult(dmg, caster->IsPlayerRef());
					const auto type = GetDefeated(target, caster, health <= fabs(dmg) + 2);
					if (type != HitResult::Proceed && Kudasai::Zone::registerdefeat(target, caster)) {
						Defeat::SetDamageImmune(target);
						RemoveDamagingSpells(target);
						if (type == HitResult::Lethal) {
							dmg = health - 0.05f;
						}
						target->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, RE::ActorValue::kHealth, -dmg);
						return;
					} else if (effect.spell->GetSpellType() != RE::MagicSystem::SpellType::kEnchantment) {
						ValidateStrip(target);
					}
				}
			}
			break;
		}
		return _MagicHit(unk1, effect, unk3, unk4, unk5);
	}

	// return false if hit should not be processed
	bool Hooks::ExplosionHit(RE::Explosion& a_explosion, float* a_flt, RE::Actor* a_actor)
	{
		if (a_actor && Defeat::IsDamageImmune(a_actor))
			return false;

		return _ExplosionHit(a_explosion, a_flt, a_actor);
	}

	bool Hooks::IsMagicImmune(RE::Actor* target, RE::MagicItem* item)
	{
		if (!target || !item || target->IsDead() || !Defeat::IsDamageImmune(target))
			return _IsMagicImmune(target, item);

		for (auto& effect : item->effects) {
			auto base = effect ? effect->baseEffect : nullptr;
			if (!base)
				continue;
			if (base->data.archetype != RE::EffectSetting::Archetype::kScript) {
				if (base->data.primaryAV == RE::ActorValue::kHealth || base->data.secondaryAV == RE::ActorValue::kHealth)
					return base->data.flags.underlying() & 4;  // Some positive effect on Hp
				return true;
			}
		}
		return _IsMagicImmune(target, item);
	}

	uint8_t* Hooks::DoDetect(RE::Actor* viewer, RE::Actor* target, int32_t& detectval, uint8_t& unk04, uint8_t& unk05, uint32_t& unk06, RE::NiPoint3& pos, float& unk08, float& unk09, float& unk10)
	{
		if (viewer && target && (Defeat::ispacified(viewer) || Defeat::ispacified(target))) {
			detectval = -1000;
			return nullptr;
		}
		return _DoDetect(viewer, target, detectval, unk04, unk05, unk06, pos, unk08, unk09, unk10);
	}

	Hooks::HitResult Hooks::GetDefeated(RE::Actor* a_victim, RE::Actor* a_aggressor, const bool lethal)
	{
		if (!ValidPair(a_victim, a_aggressor))
			return HitResult::Proceed;

		static const auto hunterpride = RE::TESDataHandler::GetSingleton()->LookupForm<RE::EffectSetting>(MgEffHunterPride, ESPNAME);
		if (a_aggressor->IsPlayerRef()) {
			if (!a_aggressor->HasMagicEffect(hunterpride) || !lethal)
				return HitResult::Proceed;
		} else if (a_aggressor->IsCommandedActor()) {
			auto tmp = a_aggressor->GetCommandingActor();
			if (tmp && tmp->IsPlayerRef())
				if (!tmp->HasMagicEffect(hunterpride) || !lethal)
					return HitResult::Proceed;
		}

		const auto settings = Papyrus::Settings::GetSingleton();
		if (lethal) {
			using Flag = RE::Actor::BOOL_FLAGS;
			bool protecc;
			if (settings->bLethalEssential && (a_victim->boolFlags.all(Flag::kEssential) || (!a_aggressor->IsPlayerRef() && a_victim->boolFlags.all(Flag::kProtected))))
				protecc = true;
			else if (a_victim->IsPlayerRef())
				protecc = Random::draw<float>(0, 99.5f) < settings->fLethalPlayer;
			else
				protecc = Random::draw<float>(0, 99.5f) < settings->fLethalNPC;
			return protecc ? HitResult::Lethal : HitResult::Proceed;
		} else {
			// only allow NPC to be defeated through this
			if (Papyrus::Configuration::IsNPC(a_victim) || a_victim->HasKeyword(RE::TESForm::LookupByID<RE::BGSKeyword>(0x04035538))) {
				const auto reqmissing = settings->iStripReq;
				if (reqmissing > 0 && Random::draw<float>(0, 99.5) < settings->fStripReqChance) {
					const auto gear = GetWornArmor(a_victim, false);
					constexpr uint32_t ignoredslots{ (1U << 1) + (1U << 5) + (1U << 6) + (1U << 9) + (1U << 11) + (1U << 12) + (1U << 13) + (1U << 15) + (1U << 20) + (1U << 21) + (1U << 31) };
					const auto occupiedslots = [&gear]() {
						static const auto nostrip = RE::TESDataHandler::GetSingleton()->LookupForm<RE::BGSKeyword>(KeywordNoStrip, ESPNAME);
						uint32_t ret = 0;
						for (auto& e : gear) {
							if (e->HasKeyword(nostrip))
								continue;
							ret += static_cast<uint32_t>(e->GetSlotMask());
						}
						return ret;
					}();
					auto t = std::popcount(occupiedslots & (~ignoredslots));
					if (t < reqmissing)
						return HitResult::Defeat;
				}
			}
		}
		return HitResult::Proceed;
	}

	const float Hooks::GetTaperDamage(const float magnitude, const RE::EffectSetting::EffectSettingData& data)
	{
		return magnitude * data.taperWeight * data.taperDuration / (data.taperCurve + 1);
	}

	const float Hooks::GetIncomingEffectDamage(RE::Actor* subject)
	{
		const auto effects = subject->GetActiveEffectList();
		if (!effects)
			return 0.0f;

		float ret = 0.0f;
		for (const auto& effect : *effects) {
			if (!effect || effect->flags.any(RE::ActiveEffect::Flag::kDispelled, RE::ActiveEffect::Flag::kInactive))
				continue;
			else if (const float change = GetExpectedHealthModification(effect); change != 0.0f) {
				ret += change;
			}
		}
		// if total health mod is greater 0 were getting healed, but we only want damage here
		return ret < 0 ? ret : 0;
	}

	const float Hooks::GetExpectedHealthModification(RE::ActiveEffect* a_effect)
	{
		const auto base = a_effect->GetBaseObject();
		if (!base || base->data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kRecover))
			return 0.0f;
		// Damage done every second by the effect
		const auto getmagnitude = [&]() -> float {
			if (a_effect->duration - base->data.taperDuration < a_effect->elapsedSeconds)
				return GetTaperDamage(a_effect->magnitude, base->data);
			else
				return a_effect->magnitude;
		};
		if (base->data.primaryAV == RE::ActorValue::kHealth)
			return getmagnitude();
		else if (base->data.secondaryAV == RE::ActorValue::kHealth)	 // only DualValueModifier can have an ActorValue as 2nd Item
			return getmagnitude() * base->data.secondAVWeight;
		else
			return 0.0f;
	}

	void Hooks::RemoveDamagingSpells(RE::Actor* subject)
	{
		auto effects = subject->GetActiveEffectList();
		if (!effects)
			return;

		for (auto& eff : *effects) {
			if (!eff || eff->flags.all(RE::ActiveEffect::Flag::kDispelled))
				continue;
			auto base = eff->GetBaseObject();
			if (!base)
				continue;
			if (IsDamagingSpell(base->data)) {
				logger::info("Dispelling Spell = {}", base->GetFormID());
				eff->Dispel(true);
			} else if (base->data.archetype == RE::EffectSetting::Archetype::kCloak) {
				const auto associate = base->data.associatedForm;
				if (associate == nullptr)
					continue;
				const auto magicitem = associate->As<RE::MagicItem>();
				for (auto& e : magicitem->effects) {
					if (e && e->baseEffect && IsDamagingSpell(e->baseEffect->data)) {
						eff->Dispel(true);
						break;
					}
				}
			}
		}
	}

	bool Hooks::IsDamagingSpell(const RE::EffectSetting::EffectSettingData& data)
	{
		if (data.primaryAV == RE::ActorValue::kHealth || data.secondaryAV == RE::ActorValue::kHealth)
			return (data.flags.underlying() & 6) == 4;
		return false;
	}

	bool Hooks::ValidPair(RE::Actor* a_victim, RE::Actor* a_aggressor)
	{
		static const auto ActorTypeGhost = RE::TESForm::LookupByID<RE::BGSKeyword>(0x000D205E);
		if (a_victim->HasKeyword(ActorTypeGhost))
			return false;
		if (!Papyrus::Configuration::IsNPC(a_aggressor) && !Papyrus::Settings::GetSingleton()->bCreatureDefeat)
			return false;
		if (!a_aggressor->IsPlayerRef() && !a_victim->IsHostileToActor(a_aggressor))
			return false;
		return ValidContender(a_victim) && ValidContender(a_aggressor);
	}

	bool Hooks::ValidContender(RE::Actor* a_actor)
	{
		if (a_actor->IsDead())
			return false;
		return Config::IsValidActor(a_actor);
	}

	void Hooks::AdjustByDifficultyMult(float& damage, const bool playerPOV)
	{
		const auto s = RE::GetINISetting("iDifficulty:GamePlay");
		if (s->GetType() != RE::Setting::Type::kSignedInteger)
			return;

		std::string diff{ "fDiffMultHP"s + (playerPOV ? "ByPC"s : "ToPC"s) };
		switch (s->GetSInt()) {
		case 0:
			diff.append("VE");
			break;
		case 1:
			diff.append("E");
			break;
		case 2:
			diff.append("N");
			break;
		case 3:
			diff.append("H");
			break;
		case 4:
			diff.append("VH");
			break;
		case 5:
			diff.append("L");
			break;
		default:
			logger::error("Invalid Difficulty Setting");
			return;
		}
		const auto smult = RE::GameSettingCollection::GetSingleton()->GetSetting(diff.c_str());
		if (smult->GetType() != RE::Setting::Type::kFloat)
			return;

		const auto mult = smult->GetFloat();
		damage *= mult;
	}

	RE::Actor* Hooks::GetNearValidAggressor(RE::Actor* victim)
	{
		const auto& tarP = victim->GetPosition();
		const auto valid = [&](RE::Actor* subject) {
			return subject->IsHostileToActor(victim) && subject->IsInCombat() && subject->GetPosition().GetDistance(tarP) < 4096.0f;
		};
		const auto& processLists = RE::ProcessLists::GetSingleton();
		for (auto& pHandle : processLists->highActorHandles) {
			auto potential = pHandle.get().get();
			if (!potential || potential->IsDead() || Defeat::IsDamageImmune(potential) || !valid(potential))
				continue;
			if (const auto group = potential->GetCombatGroup(); group) {
				for (auto& e : group->targets)
					if (e.targetHandle.get().get() == victim)
						return potential;
			}
		}
		const auto player = RE::PlayerCharacter::GetSingleton();
		return valid(player) ? player : nullptr;
	}

	void Hooks::ValidateStrip(RE::Actor* a_victim)
	{
		const auto settings = Papyrus::Settings::GetSingleton();
		if (Random::draw<float>(0, 99.5f) >= settings->fStripChance)
			return;

		static std::vector<RE::FormID> cache{};
		if (std::find(cache.begin(), cache.end(), a_victim->formID) != cache.end())
			return;
		const auto id = a_victim->formID;
		cache.push_back(id);
		std::thread([id]() {
			std::this_thread::sleep_for(std::chrono::seconds(3));
			cache.erase(std::find(cache.begin(), cache.end(), id));
		}).detach();
		const auto gear = GetWornArmor(a_victim, false);
		if (gear.empty())
			return;
		const auto item = gear.at(Random::draw<size_t>(0, gear.size() - 1));
		static const auto nostrip = RE::TESDataHandler::GetSingleton()->LookupForm<RE::BGSKeyword>(KeywordNoStrip, ESPNAME);
		if (item->HasKeyword(nostrip))
			return;
		RE::ActorEquipManager::GetSingleton()->UnequipObject(a_victim, item, nullptr, 1, nullptr, true, false, false, true);
		if (Random::draw<float>(0, 99.5f) < settings->fStripDestroy && !Papyrus::Configuration::IsStripProtecc(item)) {
			if (a_victim->IsPlayerRef() && Papyrus::GetSetting<bool>("bNotifyDestroy")) {
				if (Papyrus::GetSetting<bool>("bNotifyColored")) {
					auto color = Papyrus::GetSetting<RE::BSFixedString>("sNotifyColorChoice");
					RE::DebugNotification(fmt::format("<font color = '{}'>{} got teared off and destroyed</font color>", color, item->GetName()).c_str());
				} else {
					RE::DebugNotification(fmt::format("{} got teared off and destroyed", item->GetName()).c_str());
				}
			}
			a_victim->RemoveItem(item, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
		} else if (settings->bStripDrop) {
			a_victim->RemoveItem(item, 1, RE::ITEM_REMOVE_REASON::kDropping, nullptr, nullptr);
		} else if (!a_victim->IsPlayerRef()) {
			auto& v = EventHandler::GetSingleton()->worn_cache;
			if (auto where = v.find(a_victim->GetFormID()); where != v.end())
				where->second.push_back(item);
			else
				v.insert(std::make_pair(a_victim->GetFormID(), std::vector<RE::TESObjectARMO*>{ item }));
		}
	}
}  // namespace Hooks
