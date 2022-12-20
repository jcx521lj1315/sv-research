#include <QFile>
#include <windows.h>
#include <xmmintrin.h>
#include <cassert>
#include <memory>
#include "Xoroshiro128Plus.hpp"
#include "SeedFinder.hpp"

SeedFinder::SeedFinder() :
	hFinderThread(NULL),
	thread_count(1),
	game(GameScarlet),
	min_seed(0),
	max_seed(0),
	stars(0),
	species(0),
	tera_type(0),
	ability(0),
	nature(0),
	gender(0),
	shiny(0),
	item_filters_active(false),
	drop_threshold(0),
	item_filters_count(0)
{
	memset(min_iv, 0, sizeof(min_iv));
	memset(max_iv, 0, sizeof(max_iv));
	memset(min_iv_vec, 0, sizeof(min_iv_vec));
	memset(max_iv_vec, 0, sizeof(max_iv_vec));
}

bool SeedFinder::initialize()
{
	std::vector<uint8_t> encounter_data = read_file(":/RaidCalc/encounter_gem_paldea.pkl");
	std::vector<uint8_t> reward_map = read_file(":/RaidCalc/reward_map");
	if (encounter_data.empty() || reward_map.empty())
		return false;
	size_t encounter_count = encounter_data.size() / 0x18;
	size_t rewards_count = reward_map.size() / 0x10;
	if (encounter_count != rewards_count)
		return false;
	compute_fast_lottery_lookups();
	encounters.resize(7);
	uint64_t* rewards = (uint64_t*)reward_map.data();
	for (size_t i = 0; i < encounter_count; ++i, rewards += 2)
	{
		auto enc = EncounterTera9(encounter_data.data() + i * 0x18);
		enc.fixed_drops = get_fixed_drop_table(rewards[0]);
		enc.lottery_drops = get_lottery_drop_table(rewards[1], enc.lottery_lookup);
		assert(enc.stars > 0 && enc.stars < 7);
		encounters[enc.stars].push_back(enc);
	}
	compute_fast_encounter_lookups();
	target_drops.resize(20000 + 1);
	return true;
}

std::vector<uint8_t> SeedFinder::read_file(const char *filename) const
{
	QFile file(filename);
	if (!file.open(QIODevice::ReadOnly))
		return std::vector<uint8_t>();
	std::vector<uint8_t> buffer;
	buffer.resize(file.size());
	QDataStream stream(&file);
	if (stream.readRawData((char *)buffer.data(), buffer.size()) != buffer.size())
		return std::vector<uint8_t>();
	return buffer;
}

const RaidFixedRewards* SeedFinder::get_fixed_drop_table(uint64_t table_name) const
{
	for (auto &table : fixed_rewards)
		if (table.table_name == table_name)
			return &table;
	return nullptr;
}

const RaidLotteryRewards* SeedFinder::get_lottery_drop_table(uint64_t table_name, const uint8_t*& fast_lookup) const
{
	for (size_t i = 0; i < _countof(lottery_rewards); ++i)
	{
		auto& table = lottery_rewards[i];
		if (table.table_name == table_name)
		{
			fast_lookup = fast_lottery_lookup[i].data();
			return &table;
		}
	}
	return nullptr;
}

void SeedFinder::compute_fast_lottery_lookups()
{
	fast_lottery_lookup.resize(_countof(lottery_rewards));
	for (size_t i = 0; i < _countof(lottery_rewards); ++i)
	{
		auto& record = lottery_rewards[i];
		for (int32_t roll = 0; roll < record.rate_total; ++roll)
		{
			int32_t tmp_roll = roll;
			for (uint8_t j = 0; j < _countof(record.items); ++j)
			{
				auto& item = record.items[j];
				if (tmp_roll < item.rate)
				{
					fast_lottery_lookup[i].push_back(j);
					break;
				}
				tmp_roll -= item.rate;
			}
		}
	}
}

void SeedFinder::compute_fast_encounter_lookups()
{
	for (int32_t ver = GameScarlet; ver <= GameViolet; ++ver)
	{
		auto& lookup = fast_encounter_lookup[ver];
		lookup.resize(encounters.size());
		for (size_t stars = 1; stars < encounters.size(); ++stars)
		{
			uint64_t total = get_rate_total_base(ver, stars);
			for (uint64_t speciesroll = 0; speciesroll < total; ++speciesroll)
			{
				for (size_t i = 0; i < encounters[stars].size(); ++i)
				{
					EncounterTera9& enc = encounters[stars][i];
					int16_t minimum = enc.rand_rate_min[ver];
					if (minimum >= 0 && (uint32_t)((int32_t)speciesroll - minimum) < enc.rand_rate)
					{
						assert(i <= 0xFF);
						lookup[stars].push_back((uint8_t)i);
						break;
					}
				}
			}
		}
	}
}

FORCEINLINE int16_t SeedFinder::get_rate_total_base(int32_t version, size_t star) const
{
	static const int16_t rates[2][7] =
	{
		{ 0, 5800, 5300, 7400, 8800, 9100, 6500 },
		{ 0, 5800, 5300, 7400, 8700, 9100, 6500 },
	};
	assert(star > 0 && star < _countof(rates[0]));
	return rates[version][star];
};

FORCEINLINE const EncounterTera9* SeedFinder::get_encounter(uint32_t seed, int stage) const
{
	Xoroshiro128Plus gen(seed);
	if (stars < 6)
		gen.next_int(100); // difficulty roll
	uint64_t total = get_rate_total_base(game, stars);
	uint64_t speciesroll = gen.next_int(total);
	return &encounters[stars][fast_encounter_lookup[game][stars][speciesroll]];
}

FORCEINLINE int32_t SeedFinder::get_reward_count(int32_t random, int32_t stars) const
{
	static const int32_t reward_slots[8][5] =
	{
		{ 0, 0, 0, 0, 0 },
		{ 4, 5, 6, 7, 8 },
		{ 4, 5, 6, 7, 8 },
		{ 5, 6, 7, 8, 9 },
		{ 5, 6, 7, 8, 9 },
		{ 6, 7, 8, 9, 10 },
		{ 7, 8, 9, 10, 11 },
		{ 7, 8, 9, 10, 11 },
	};
	assert(stars > 0 && stars < _countof(reward_slots));
	static const int8_t random_lookup[] =
	{
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
		2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
		2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
		2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
		3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
		4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	};
	assert(random < _countof(random_lookup));
	return reward_slots[stars][random_lookup[random]];
}

FORCEINLINE uint32_t SeedFinder::get_rewards(uint32_t seed, int progress, int raid_boost) const
{
	uint32_t drop_counter = 0;
	const EncounterTera9 *enc = get_encounter(seed, progress);

	auto& fixed_items = enc->fixed_drops->items;
	#define add_fixed_drop(n) drop_counter += fixed_items[n].num & target_drops[fixed_items[n].item_id]
	add_fixed_drop(0);
	add_fixed_drop(1);
	add_fixed_drop(2);
	add_fixed_drop(3);
	add_fixed_drop(4);
	add_fixed_drop(5);
	add_fixed_drop(6);
	#undef add_fixed_drop

	Xoroshiro128Plus gen(seed);
	int32_t rate_total = enc->lottery_drops->rate_total;
	int32_t count = get_reward_count((int32_t)gen.next_int(100), enc->stars) + raid_boost;
	for (int32_t i = 0; i < count; ++i)
	{
		int32_t roll = (int32_t)gen.next_int((uint64_t)rate_total);
		auto& item = enc->lottery_drops->items[enc->lottery_lookup[roll]];
		drop_counter += item.num & target_drops[item.item_id];
	}
	return drop_counter;
}

FORCEINLINE bool SeedFinder::check_pokemon(uint32_t seed) const
{
	Xoroshiro128Plus gen(seed);
	uint32_t EC = (uint32_t)gen.next_int();
	uint32_t TIDSID = (uint32_t)gen.next_int();
	uint32_t PID = (uint32_t)gen.next_int();
	bool is_shiny = (((PID >> 16) ^ (PID & 0xFFFF)) >> 4) == (((TIDSID >> 16) ^ (TIDSID & 0xFFFF)) >> 4);
	bool cond_shiny[] = { is_shiny, true, false };
	if (is_shiny != cond_shiny[shiny])
		return false;
	const EncounterTera9* enc = get_encounter(seed, StoryProgress);
	if (species && enc->species != species)
		return false;
	int8_t ivs[16] = { -1, -1, -1, -1, -1, -1 };
	for (uint8_t i = 0; i < enc->flawless_iv_count; ++i)
	{
		int32_t index;
		do
		{
			index = (int32_t)gen.next_int(6);
		} while (ivs[index] != -1);
		ivs[index] = 31;
	}
	for (size_t i = 0; i < 6; ++i)
	{
		if (ivs[i] == -1)
			ivs[i] = (int8_t)gen.next_int(32);
	}
	__m128i min_result = _mm_cmplt_epi8(*(__m128i*)ivs, *(__m128i*)min_iv_vec);
	__m128i max_result = _mm_cmpgt_epi8(*(__m128i*)ivs, *(__m128i*)max_iv_vec);
	int result = _mm_testz_si128(min_result, min_result) + _mm_testz_si128(max_result, max_result);
	if (result != 2)
		return false;
	return true;
}

FORCEINLINE bool SeedFinder::check_rewards(uint32_t seed) const
{
	uint32_t drops = get_rewards(seed, StoryProgress, RaidBoost);
	if (drops < drop_threshold)
		return false;
	return true;
}

void SeedFinder::combo_thread(ThreadData& data)
{
	for (uint64_t seed = data.range_min; seed < data.range_max; ++seed)
	{
		if (check_pokemon(seed) && check_rewards(seed))
			data.results.push_back(seed);
	}
}

DWORD WINAPI SeedFinder::combo_thread_wrapper(LPVOID Parameter)
{
	ThreadData* data = (ThreadData*)Parameter;
	data->finder->combo_thread(*data);
	return 0;
}

void SeedFinder::rewards_thread(ThreadData& data)
{
	for (uint64_t seed = data.range_min; seed < data.range_max; ++seed)
	{
		if (check_rewards(seed))
			data.results.push_back(seed);
	}
}

DWORD WINAPI SeedFinder::rewards_thread_wrapper(LPVOID Parameter)
{
	ThreadData* data = (ThreadData*)Parameter;
	data->finder->rewards_thread(*data);
	return 0;
}

void SeedFinder::pokemon_thread(ThreadData& data)
{
	for (uint64_t seed = data.range_min; seed < data.range_max; ++seed)
	{
		if (check_pokemon(seed))
			data.results.push_back(seed);
	}
}

DWORD WINAPI SeedFinder::pokemon_thread_wrapper(LPVOID Parameter)
{
	ThreadData* data = (ThreadData*)Parameter;
	data->finder->pokemon_thread(*data);
	return 0;
}

void SeedFinder::find_seeds_thread()
{
	auto handles = std::make_unique<HANDLE[]>(thread_count);
	auto thread_data = std::make_unique<ThreadData[]>(thread_count);
	uint64_t seed_count = max_seed - min_seed + 1ULL;
	uint64_t seed_chunk = seed_count / thread_count;
	LPTHREAD_START_ROUTINE proc = NULL;
	if (use_pokemon_filters() && use_item_filters())
		proc = combo_thread_wrapper;
	else if (use_item_filters())
		proc = rewards_thread_wrapper;
	else
		proc = pokemon_thread_wrapper;
	time_taken.start();
	for (uint32_t i = 0; i < thread_count; ++i)
	{
		auto& data = thread_data[i];
		data.finder = this;
		data.range_min = min_seed + seed_chunk * i;
		data.range_max = min_seed + seed_chunk * (i + 1);
		if (i == thread_count - 1)
			data.range_max = min_seed + seed_count;
		handles[i] = CreateThread(NULL, 0, proc, &data, 0, NULL);
		if (!handles[i])
		{
			if (i > 0)
			{
				WaitForMultipleObjects(i, handles.get(), TRUE, INFINITE);
				for (int32_t j = 0; j < i; ++j)
					CloseHandle(handles[j]);
			}
			time_taken.stop();
			return;
		}
	}
	DWORD result = WaitForMultipleObjects(thread_count, handles.get(), TRUE, INFINITE);
	time_taken.stop();
	for (uint32_t i = 0; i < thread_count; ++i)
		CloseHandle(handles[i]);
	bool success = result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + thread_count;
	if (!success)
		return;
	for (uint32_t i = 0; i < thread_count; ++i)
	{
		auto& data = thread_data[i];
		seeds.insert(seeds.end(), data.results.begin(), data.results.end());
	}
}

DWORD WINAPI SeedFinder::find_seeds_thread_wrapper(LPVOID Parameter)
{
	SeedFinder* finder = (SeedFinder*)Parameter;
	finder->find_seeds_thread();
	return 0;
}

bool SeedFinder::find_seeds()
{
	seeds.clear();
	memcpy(min_iv_vec, min_iv, sizeof(min_iv));
	memcpy(max_iv_vec, max_iv, sizeof(max_iv));
	hFinderThread = CreateThread(NULL, 0, find_seeds_thread_wrapper, this, 0, NULL);
	if (!hFinderThread)
		return false;
	return true;
}

bool SeedFinder::is_search_done()
{
	if (!hFinderThread)
		return true;
	DWORD wait_result = WaitForSingleObject(hFinderThread, 0);
	if (wait_result != WAIT_OBJECT_0)
		return false;
	CloseHandle(hFinderThread);
	hFinderThread = NULL;
	return true;
}

void SeedFinder::set_drop_filter(int item_id, bool value)
{
	auto& drop_value = target_drops[item_id];
	if (value)
	{
		if (!drop_value)
			++item_filters_count;
		drop_value = -1;
	}
	else
	{
		if (drop_value)
			--item_filters_count;
		drop_value = 0;
	}
}

bool SeedFinder::use_pokemon_filters() const
{
	if (species != 0 || shiny != 0)
		return true;
	for (auto iv : min_iv)
		if (iv != 0)
			return true;
	for (auto iv : max_iv)
		if (iv != 31)
			return true;
	return false;
}

bool SeedFinder::use_item_filters() const
{
	return item_filters_active && item_filters_count > 0;
}

bool SeedFinder::use_filters() const
{
	return use_pokemon_filters() || use_item_filters();
}

SeedFinder::SeedInfo SeedFinder::get_seed_info(uint32_t seed) const
{
	SeedInfo info;
	info.seed = seed;
	Xoroshiro128Plus gen(seed);
	info.ec = (uint32_t)gen.next_int();
	uint32_t TIDSID = (uint32_t)gen.next_int();
	info.pid = (uint32_t)gen.next_int();
	info.shiny = (((info.pid >> 16) ^ (info.pid & 0xFFFF)) >> 4) == (((TIDSID >> 16) ^ (TIDSID & 0xFFFF)) >> 4);
	const EncounterTera9* enc = get_encounter(seed, StoryProgress);
	info.species = enc->species;
	int8_t ivs[6] = { -1, -1, -1, -1, -1, -1 };
	for (uint8_t i = 0; i < enc->flawless_iv_count; ++i)
	{
		int32_t index;
		do
		{
			index = (int32_t)gen.next_int(6);
		} while (ivs[index] != -1);
		ivs[index] = 31;
	}
	for (size_t i = 0; i < _countof(ivs); ++i)
	{
		if (ivs[i] == -1)
			ivs[i] = (int8_t)gen.next_int(32);
	}
	memcpy(info.iv, ivs, sizeof(info.iv));
	if (use_item_filters())
		info.drops = get_rewards(seed, StoryProgress, RaidBoost);
	else
		info.drops = 0;
	return info;
}

void SeedFinder::visit_encounters(std::function<EncounterVisitor> visitor) const
{
	for (auto& enc_vector : encounters)
		for (auto& enc : enc_vector)
			visitor(enc);
}
