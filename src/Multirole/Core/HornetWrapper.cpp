#include "HornetWrapper.hpp"

#include <boost/date_time/posix_time/posix_time_types.hpp>

#include "IDataSupplier.hpp"
#include "IScriptSupplier.hpp"
#include "ILogger.hpp"
#include "../../HornetCommon.hpp"
#define PROCESS_IMPLEMENTATION
#include "../../Process.hpp"

namespace Ignis::Multirole::Core
{

#include "../../Read.inl"
#include "../../Write.inl"

inline std::string MakeHornetName(uintptr_t addr)
{
	std::array<char, 25U> buf;
	std::snprintf(buf.data(), buf.size(), "Hornet0x%lX", addr);
	return std::string(buf.data());
}

inline ipc::shared_memory_object MakeShm(const std::string& str)
{
	// Make sure the shared memory object doesn't exist before attempting
	// to create it again.
	ipc::shared_memory_object::remove(str.data());
	ipc::shared_memory_object shm(ipc::create_only, str.data(), ipc::read_write);
	shm.truncate(sizeof(Hornet::SharedSegment));
	return shm;
}

// public

HornetWrapper::HornetWrapper(std::string_view absFilePath) :
	shmName(MakeHornetName(reinterpret_cast<uintptr_t>(this))),
	shm(MakeShm(shmName)),
	region(shm, ipc::read_write),
	hss(nullptr)
{
	void* addr = region.get_address();
	hss = new (addr) Hornet::SharedSegment();
	const auto p = Process::Launch("./hornet", absFilePath.data(), shmName.data());
	if(!p.second)
	{
		DestroySharedSegment();
		throw std::runtime_error("Unable to launch child");
	}
	proc = p.first;
	try
	{
		NotifyAndWait(Hornet::Action::HEARTBEAT);
	}
	catch(Core::Exception& e)
	{
		Process::CleanUp(proc);
		DestroySharedSegment();
		throw std::runtime_error("Heartbeat failed");
	}
}

HornetWrapper::~HornetWrapper()
{
	hss->act = Hornet::Action::EXIT;
	hss->cv.notify_one();
	Process::CleanUp(proc);
	DestroySharedSegment();
}

std::pair<int, int> HornetWrapper::Version()
{
	std::scoped_lock lock(mtx);
	NotifyAndWait(Hornet::Action::OCG_GET_VERSION);
	const auto* rptr = hss->bytes.data();
	return
	{
		Read<int>(rptr),
		Read<int>(rptr),
	};
}

IWrapper::Duel HornetWrapper::CreateDuel(const DuelOptions& opts)
{
	std::scoped_lock lock(mtx);
	auto* wptr = hss->bytes.data();
	Write<OCG_DuelOptions>(wptr,
	{
		opts.seed,
		opts.flags,
		opts.team1,
		opts.team2,
		nullptr, // NOTE: Set on Hornet
		&opts.dataSupplier,
		nullptr, // NOTE: Set on Hornet
		&opts.scriptSupplier,
		nullptr, // NOTE: Set on Hornet
		opts.optLogger,
		nullptr, // NOTE: Set on Hornet
		&opts.dataSupplier
	});
	NotifyAndWait(Hornet::Action::OCG_CREATE_DUEL);
	const auto* rptr = hss->bytes.data();
	if(Read<int>(rptr) != OCG_DUEL_CREATION_SUCCESS)
		throw Core::Exception("OCG_CreateDuel failed!");
	return Read<OCG_Duel>(rptr);
}

void HornetWrapper::DestroyDuel(Duel duel)
{
	std::scoped_lock lock(mtx);
	auto* wptr = hss->bytes.data();
	Write<OCG_Duel>(wptr, duel);
	NotifyAndWait(Hornet::Action::OCG_DESTROY_DUEL);
}

void HornetWrapper::AddCard(Duel duel, const OCG_NewCardInfo& info)
{
	std::scoped_lock lock(mtx);
	auto* wptr = hss->bytes.data();
	Write<OCG_Duel>(wptr, duel);
	Write<OCG_NewCardInfo>(wptr, info);
	NotifyAndWait(Hornet::Action::OCG_DUEL_NEW_CARD);
}

void HornetWrapper::Start(Duel duel)
{
	std::scoped_lock lock(mtx);
	auto* wptr = hss->bytes.data();
	Write<OCG_Duel>(wptr, duel);
	NotifyAndWait(Hornet::Action::OCG_START_DUEL);
}

IWrapper::DuelStatus HornetWrapper::Process(Duel duel)
{
	std::scoped_lock lock(mtx);
	auto* wptr = hss->bytes.data();
	Write<OCG_Duel>(wptr, duel);
	NotifyAndWait(Hornet::Action::OCG_DUEL_PROCESS);
	const auto* rptr = hss->bytes.data();
	return DuelStatus{Read<int>(rptr)};
}

IWrapper::Buffer HornetWrapper::GetMessages(Duel duel)
{
	std::scoped_lock lock(mtx);
	auto* wptr = hss->bytes.data();
	Write<OCG_Duel>(wptr, duel);
	NotifyAndWait(Hornet::Action::OCG_DUEL_GET_MESSAGE);
	const auto* rptr = hss->bytes.data();
	const auto size = static_cast<std::size_t>(Read<uint32_t>(rptr));
	Buffer buffer(size);
	std::memcpy(buffer.data(), rptr, size);
	return buffer;
}

void HornetWrapper::SetResponse(Duel duel, const Buffer& buffer)
{
	std::scoped_lock lock(mtx);
	auto* wptr = hss->bytes.data();
	Write<OCG_Duel>(wptr, duel);
	Write<std::size_t>(wptr, buffer.size());
	std::memcpy(wptr, buffer.data(), buffer.size());
	NotifyAndWait(Hornet::Action::OCG_DUEL_SET_RESPONSE);
}

int HornetWrapper::LoadScript(Duel duel, std::string_view name, std::string_view str)
{
	std::scoped_lock lock(mtx);
	auto* wptr = hss->bytes.data();
	Write<OCG_Duel>(wptr, duel);
	Write<std::size_t>(wptr, name.size());
	std::memcpy(wptr, name.data(), name.size());
	wptr += name.size();
	Write<std::size_t>(wptr, str.size());
	std::memcpy(wptr, str.data(), str.size());
	NotifyAndWait(Hornet::Action::OCG_LOAD_SCRIPT);
	const auto* rptr = hss->bytes.data();
	return Read<int>(rptr);
}

std::size_t HornetWrapper::QueryCount(Duel duel, uint8_t team, uint32_t loc)
{
	std::scoped_lock lock(mtx);
	auto* wptr = hss->bytes.data();
	Write<OCG_Duel>(wptr, duel);
	Write<uint8_t>(wptr, team);
	Write<uint32_t>(wptr, loc);
	NotifyAndWait(Hornet::Action::OCG_DUEL_QUERY_COUNT);
	const auto* rptr = hss->bytes.data();
	return static_cast<std::size_t>(Read<uint32_t>(rptr));
}

IWrapper::Buffer HornetWrapper::Query(Duel duel, const QueryInfo& info)
{
	std::scoped_lock lock(mtx);
	auto* wptr = hss->bytes.data();
	Write<OCG_Duel>(wptr, duel);
	Write<OCG_QueryInfo>(wptr, info);
	NotifyAndWait(Hornet::Action::OCG_DUEL_QUERY);
	const auto* rptr = hss->bytes.data();
	const auto size = static_cast<std::size_t>(Read<uint32_t>(rptr));
	Buffer buffer(size);
	std::memcpy(buffer.data(), rptr, size);
	return buffer;
}

IWrapper::Buffer HornetWrapper::QueryLocation(Duel duel, const QueryInfo& info)
{
	std::scoped_lock lock(mtx);
	auto* wptr = hss->bytes.data();
	Write<OCG_Duel>(wptr, duel);
	Write<OCG_QueryInfo>(wptr, info);
	NotifyAndWait(Hornet::Action::OCG_DUEL_QUERY_LOCATION);
	const auto* rptr = hss->bytes.data();
	const auto size = static_cast<std::size_t>(Read<uint32_t>(rptr));
	Buffer buffer(size);
	std::memcpy(buffer.data(), rptr, size);
	return buffer;
}

IWrapper::Buffer HornetWrapper::QueryField(Duel duel)
{
	std::scoped_lock lock(mtx);
	auto* wptr = hss->bytes.data();
	Write<OCG_Duel>(wptr, duel);
	NotifyAndWait(Hornet::Action::OCG_DUEL_QUERY_FIELD);
	const auto* rptr = hss->bytes.data();
	auto size = static_cast<std::size_t>(Read<uint32_t>(rptr));
	Buffer buffer(size);
	std::memcpy(buffer.data(), rptr, size);
	return buffer;
}

void HornetWrapper::DestroySharedSegment()
{
	hss->~SharedSegment();
	ipc::shared_memory_object::remove(shmName.data());
}

void HornetWrapper::NotifyAndWait(Hornet::Action act)
{
	// Time to wait before checking for process being dead
	auto NowPlusOffset = []() -> boost::posix_time::ptime
	{
		using namespace boost::posix_time;
		return microsec_clock::universal_time() + milliseconds(125);
	};
	hss->act = act;
	hss->cv.notify_one();
	{
		Hornet::LockType lock(hss->mtx);
		while(!hss->cv.timed_wait(lock, NowPlusOffset(), [&](){return hss->act != act;}))
		{
			if(Process::IsRunning(proc))
				continue;
			throw Core::Exception("Hornet hanged!");
		}
	}
	if(hss->act == Hornet::Action::NO_WORK)
		return;
	switch(hss->act)
	{
		case Hornet::Action::CB_DATA_READER:
		{
			const auto* rptr = hss->bytes.data();
			auto* supplier = static_cast<IDataSupplier*>(Read<void*>(rptr));
			const OCG_CardData data = supplier->DataFromCode(Read<uint32_t>(rptr));
			auto* wptr = hss->bytes.data();
			Write<OCG_CardData>(wptr, data);
			for(uint16_t* wptr2 = data.setcodes; *wptr2 != 0U; wptr2++)
				Write<uint16_t>(wptr, *wptr2);
			Write<uint16_t>(wptr, 0U);
			NotifyAndWait(Hornet::Action::CB_DONE);
			break;
		}
		case Hornet::Action::CB_SCRIPT_READER:
		{
			const auto* rptr = hss->bytes.data();
			auto* supplier = static_cast<IScriptSupplier*>(Read<void*>(rptr));
			const auto nameSz = Read<std::size_t>(rptr);
			const std::string_view nameSv(reinterpret_cast<const char*>(rptr), nameSz);
			std::string script = supplier->ScriptFromFilePath(nameSv);
			auto* wptr = hss->bytes.data();
			Write<std::size_t>(wptr, script.size());
			if(!script.empty())
				std::memcpy(wptr, script.data(), script.size());
			NotifyAndWait(Hornet::Action::CB_DONE);
			break;
		}
		case Hornet::Action::CB_LOG_HANDLER:
		{
			const auto* rptr = hss->bytes.data();
			auto* logger = static_cast<ILogger*>(Read<void*>(rptr));
			const auto type = ILogger::LogType{Read<int>(rptr)};
			const auto strSz = Read<std::size_t>(rptr);
			const std::string_view strSv(reinterpret_cast<const char*>(rptr), strSz);
			if(logger != nullptr)
				logger->Log(type, strSv);
			NotifyAndWait(Hornet::Action::CB_DONE);
			break;
		}
		case Hornet::Action::CB_DATA_READER_DONE:
		{
			const auto* rptr = hss->bytes.data();
			auto* supplier = static_cast<IDataSupplier*>(Read<void*>(rptr));
			const auto data = Read<OCG_CardData>(rptr);
			supplier->DataUsageDone(data);
			NotifyAndWait(Hornet::Action::CB_DONE);
			break;
		}
		// Explicitly ignore these, in case we ever add more functionality...
		case Hornet::Action::NO_WORK:
		case Hornet::Action::HEARTBEAT:
		case Hornet::Action::EXIT:
		case Hornet::Action::OCG_GET_VERSION:
		case Hornet::Action::OCG_CREATE_DUEL:
		case Hornet::Action::OCG_DESTROY_DUEL:
		case Hornet::Action::OCG_DUEL_NEW_CARD:
		case Hornet::Action::OCG_START_DUEL:
		case Hornet::Action::OCG_DUEL_PROCESS:
		case Hornet::Action::OCG_DUEL_GET_MESSAGE:
		case Hornet::Action::OCG_DUEL_SET_RESPONSE:
		case Hornet::Action::OCG_LOAD_SCRIPT:
		case Hornet::Action::OCG_DUEL_QUERY_COUNT:
		case Hornet::Action::OCG_DUEL_QUERY:
		case Hornet::Action::OCG_DUEL_QUERY_LOCATION:
		case Hornet::Action::OCG_DUEL_QUERY_FIELD:
		case Hornet::Action::CB_DONE:
			break;
	}
}

} // namespace Ignis::Multirole::Core
