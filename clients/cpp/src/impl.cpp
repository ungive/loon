#include "internal.h"
#include "loon/loon.h"

using namespace loon;

Client::Client(std::string const& address)
    : m_impl{ std::make_unique<ClientImpl>(address, std::nullopt) }
{
}

loon::Client::Client(std::string const& address, std::string const& auth)
    : m_impl{ std::make_unique<ClientImpl>(address, auth) }
{
}

void Client::start() { return m_impl->start(); }

void Client::stop() { return m_impl->stop(); }

std::shared_ptr<ContentHandle> Client::register_content(
    std::shared_ptr<loon::ContentSource> source, loon::ContentInfo const& info)
{
    return m_impl->register_content(source, info);
}

void Client::unregister_content(std::shared_ptr<ContentHandle> handle)
{
    return m_impl->unregister_content(handle);
}
