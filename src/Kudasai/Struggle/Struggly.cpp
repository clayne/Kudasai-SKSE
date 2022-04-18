#include "Kudasai/Struggle/Struggly.h"

#include "Kudasai/Animation/Animation.h"
#include "Kudasai/Interface/QTE.h"

namespace Kudasai
{
	Struggle* Struggle::CreateStruggle(CallbackFunc callback, std::vector<RE::Actor*> actors, double difficulty, StruggleType type)
	{
		try {
			Kudasai::Struggle::strugglers.push_back(std::make_unique<Kudasai::Struggle>(callback, actors, difficulty, type));
			return strugglers.back().get();
		} catch (const std::exception&) {
			throw;
		}
	}

	void Struggle::DeleteStruggle(Struggle* struggle)
	{
		const auto where = std::find_if(strugglers.begin(), strugglers.end(), [&](std::unique_ptr<Kudasai::Struggle>& ptr) { return ptr.get() == struggle; });
		strugglers.erase(where);
	}

	Struggle::Struggle(CallbackFunc callback, std::vector<RE::Actor*> actors, double difficulty, StruggleType type) :
		callback(callback), actors(actors), active(true)
	{
		if (!Papyrus::Configuration::IsNPC(actors[0]))
			throw InvalidCombination();

		const std::string racekey{ Animation::GetRaceKey(actors[1]) };
		if (racekey.empty())
			throw InvalidCombination();

		std::vector<std::string> anims{};
		anims.reserve(actors.size());
		try {
			YAML::Node root = YAML::LoadFile(CONFIGPATH("Struggle.yaml"));
			YAML::Node node = root[racekey];
			if (!node.IsDefined() || !node.IsMap())
				throw InvalidCombination();

			// IDEA: consider rotation of the two Actors to play different animations? Baka might do something for this
			// 3p+ support? multiple struggle sets?
			anims.emplace_back(node["Victim"].as<std::string>());
			anims.emplace_back(node["Aggressor"].as<std::string>());
			if (std::find(anims.begin(), anims.end(), ""s) != anims.end())
				throw InvalidCombination();				

		} catch (const std::exception& e) {
			logger::error(e.what());
			throw InvalidCombination();
		}

		_t = std::thread([this, anims, type, difficulty]() {
			Animation::PlayPaired(this->actors, anims);
			// lean in for the animation.. I guess
			std::this_thread::sleep_for(std::chrono::milliseconds(3500));

			switch (type) {
			case StruggleType::None:
				{
					std::this_thread::sleep_for(std::chrono::seconds(8));
					if (active) {
						active = false;
						this->callback(randomINT<int>(0, 99) < difficulty, this);
					}
				}
				break;
			case StruggleType::QTE:
				{
					auto hits = [&difficulty]() {
						auto ret = floor((randomINT<int>(1, 100) % 4) - difficulty);
						return ret == 0 ? randomINT<int>(4, 9) : abs(ret) + 3;
					}();
					Interface::QTE::time = difficulty;
					Interface::QTE::handler = [=](bool victory) mutable {
						if (!active)
							return false;

						if (victory) {
							if (--hits > 0) {
								Interface::QTE::time *= randomREAL<double>(0.85, 1.1);
								return true;
							}
						}
						// either won and 0 hits or lost
						active = false;
						this->callback(victory, this);
						return false;
					};
					Interface::QTE::OpenMenu();
				}
				break;
			}
		});
	}

	Struggle::~Struggle() noexcept
	{
		_t.get_id() != std::this_thread::get_id() ? _t.join() : _t.detach();
	}

	void Struggle::PlayBreakfree(std::vector<RE::Actor*> positions) noexcept
	{
		const std::string racekey{ Animation::GetRaceKey(positions[1]) };
		YAML::Node root = YAML::LoadFile(CONFIGPATH("Struggle.yaml"));
		YAML::Node node = root[racekey]["Breakfree"];
		if (node.IsDefined() && node.IsMap()) {
			// IDEA: consider rotation of the two Actors to play different animations? Baka might do something for this
			auto vicanim = node["Victim"].as<std::string>();
			auto agranim = node["Aggressor"].as<std::string>();
			Animation::ExitPaired(positions, { vicanim, agranim });
		} else {
			logger::info("No Struggle for Racekey = {}, falling back to default", racekey);
			ConsolePrint("[Kudasai] Struggle has no Outro. Rooting to default");
			Animation::ExitPaired(positions, { "IdleForceDefaultState"s, "IdleForceDefaultState"s });
		}
	}

	void Struggle::PlayBreakfree(std::vector<RE::Actor*> positions, std::vector<std::string> anims) noexcept
	{
		Animation::ExitPaired(positions, anims);
	}


	void Struggle::StopStruggle(RE::Actor* defeated) noexcept
	{
		if (!active)
			return;

		active = false;
		if (actors[0]->IsPlayerRef()) {
			SKSE::GetTaskInterface()->AddUITask([]() {
				Interface::QTE::CloseMenu();
			});
		}
		callback(defeated != actors[0], this);
	}

	Struggle* Struggle::FindPair(RE::Actor* subject)
	{
		for (auto& instance : strugglers)
			if (const auto end = instance->actors.end(); std::find(instance->actors.begin(), end, subject) != end)
				return instance.get();
		return nullptr;
	}

}  // namespace Kudasai
