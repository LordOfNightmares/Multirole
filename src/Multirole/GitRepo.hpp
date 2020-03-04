#ifndef GITREPO_HPP
#define GITREPO_HPP
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "IGitRepoObserver.hpp"
#include "Endpoint/Webhook.hpp"

struct git_repository;

namespace Ignis
{

namespace Multirole
{

class GitRepo final : public Endpoint::Webhook
{
public:
	using Credentials = std::pair<std::string, std::string>;

	GitRepo(asio::io_context& ioCtx, const nlohmann::json& opts);
	~GitRepo();

	// Remove copy operations and move assignment
	GitRepo(const GitRepo&) = delete;
	GitRepo& operator=(const GitRepo&) = delete;
	GitRepo& operator=(GitRepo&&) = delete;

	// Default move constructor
	GitRepo(GitRepo&& other) = default;

	void AddObserver(IGitRepoObserver& obs);
private:
	using PathVector = IGitRepoObserver::PathVector;

	const std::string token;
	const std::string remote;
	const std::string path;
	std::unique_ptr<Credentials> credPtr;
	git_repository* repo;
	std::vector<IGitRepoObserver*> observers;

	void Callback(std::string_view payload) override;

	bool CheckIfRepoExists() const;
	void Clone();
	void Fetch();
	void ResetToFetchHead();

	std::vector<std::string> GetFilesDiff() const;
	std::vector<std::string> GetTrackedFiles() const;
};

} // namespace Multirole

} // namespace Ignis

#endif // GITREPO_HPP
