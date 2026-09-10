#include "home_hub_captive_dns.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <strings.h>

namespace bluepaws::captive_dns {
namespace {

constexpr char kTag[] = "hub_captive_dns";
constexpr uint16_t kDnsPort = 53;
constexpr std::size_t kPacketBytes = 512;
constexpr uint32_t kAnswerTtlSeconds = 60;
constexpr uint16_t kMdnsPort = 5353;
constexpr char kMdnsGroup[] = "224.0.0.251";
constexpr char kLocalHostname[] = "bluepaws.local";

struct __attribute__((packed)) DnsHeader {
    uint16_t id;
    uint16_t flags;
    uint16_t question_count;
    uint16_t answer_count;
    uint16_t authority_count;
    uint16_t additional_count;
};

struct __attribute__((packed)) DnsAnswer {
    uint16_t name_pointer;
    uint16_t type;
    uint16_t dns_class;
    uint32_t ttl;
    uint16_t address_length;
    uint32_t address;
};

std::atomic_bool g_started{false};
std::atomic_bool g_mdns_started{false};

void append_u16(uint8_t *packet, std::size_t &cursor, uint16_t value)
{
    value = htons(value);
    std::memcpy(packet + cursor, &value, sizeof(value));
    cursor += sizeof(value);
}

void append_u32(uint8_t *packet, std::size_t &cursor, uint32_t value)
{
    value = htonl(value);
    std::memcpy(packet + cursor, &value, sizeof(value));
    cursor += sizeof(value);
}

bool append_name(uint8_t *packet, std::size_t capacity, std::size_t &cursor,
                 const char *name)
{
    const char *label = name;
    while (*label != '\0') {
        const char *dot = std::strchr(label, '.');
        const std::size_t length = dot == nullptr
            ? std::strlen(label) : static_cast<std::size_t>(dot - label);
        if (length == 0U || length > 63U || cursor + length + 1U >= capacity) return false;
        packet[cursor++] = static_cast<uint8_t>(length);
        std::memcpy(packet + cursor, label, length);
        cursor += length;
        if (dot == nullptr) break;
        label = dot + 1;
    }
    if (cursor >= capacity) return false;
    packet[cursor++] = 0;
    return true;
}

bool read_name(const uint8_t *packet, std::size_t received, std::size_t &cursor,
               char *name, std::size_t capacity)
{
    std::size_t source = cursor;
    std::size_t destination = 0;
    std::size_t jumps = 0;
    bool cursor_finished = false;
    while (source < received && jumps++ < 16U) {
        const uint8_t length = packet[source++];
        if (length == 0U) {
            if (!cursor_finished) cursor = source;
            if (destination >= capacity) return false;
            name[destination] = '\0';
            return true;
        }
        if ((length & 0xC0U) == 0xC0U) {
            if (source >= received) return false;
            const std::size_t pointer =
                (static_cast<std::size_t>(length & 0x3FU) << 8U) | packet[source++];
            if (pointer >= received) return false;
            if (!cursor_finished) {
                cursor = source;
                cursor_finished = true;
            }
            source = pointer;
            continue;
        }
        if (length > 63U || source + length > received ||
            destination + length + (destination == 0U ? 0U : 1U) >= capacity) {
            return false;
        }
        if (destination != 0U) name[destination++] = '.';
        std::memcpy(name + destination, packet + source, length);
        destination += length;
        source += length;
    }
    return false;
}

bool active_wifi_address(uint32_t &address)
{
    wifi_ap_record_t station{};
    const char *interface_key = esp_wifi_sta_get_ap_info(&station) == ESP_OK
        ? "WIFI_STA_DEF" : "WIFI_AP_DEF";
    esp_netif_t *network_interface = esp_netif_get_handle_from_ifkey(interface_key);
    esp_netif_ip_info_t ip_info{};
    if (network_interface == nullptr ||
        esp_netif_get_ip_info(network_interface, &ip_info) != ESP_OK ||
        ip_info.ip.addr == 0U) {
        return false;
    }
    address = ip_info.ip.addr;
    return true;
}

std::size_t make_mdns_reply(const uint8_t *query, std::size_t received,
                            uint8_t *reply, std::size_t capacity)
{
    if (received < sizeof(DnsHeader) || capacity < sizeof(DnsHeader)) return 0;
    const auto *header = reinterpret_cast<const DnsHeader *>(query);
    const uint16_t questions = ntohs(header->question_count);
    if (questions == 0U || questions > 8U) return 0;

    std::size_t cursor = sizeof(DnsHeader);
    const char *matched_name = nullptr;
    for (uint16_t index = 0; index < questions; ++index) {
        char name[96]{};
        if (!read_name(query, received, cursor, name, sizeof(name)) ||
            cursor + 4U > received) {
            return 0;
        }
        uint16_t query_type = 0;
        uint16_t query_class = 0;
        std::memcpy(&query_type, query + cursor, sizeof(query_type));
        std::memcpy(&query_class, query + cursor + 2U, sizeof(query_class));
        cursor += 4U;
        query_type = ntohs(query_type);
        query_class = ntohs(query_class) & 0x7FFFU;
        if (query_class == 1U && (query_type == 1U || query_type == 255U)) {
            if (strcasecmp(name, kLocalHostname) == 0) matched_name = kLocalHostname;
        }
    }
    uint32_t address = 0;
    if (matched_name == nullptr || !active_wifi_address(address)) return 0;

    std::memset(reply, 0, capacity);
    auto *response_header = reinterpret_cast<DnsHeader *>(reply);
    response_header->flags = htons(0x8400U);  // response + authoritative
    response_header->answer_count = htons(1U);
    cursor = sizeof(DnsHeader);
    if (!append_name(reply, capacity, cursor, matched_name) || cursor + 14U > capacity) return 0;
    append_u16(reply, cursor, 1U);       // A
    append_u16(reply, cursor, 0x8001U);  // IN + cache flush
    append_u32(reply, cursor, 120U);
    append_u16(reply, cursor, 4U);
    std::memcpy(reply + cursor, &address, sizeof(address));
    cursor += sizeof(address);
    return cursor;
}

std::size_t make_reply(uint8_t *packet, std::size_t received)
{
    if (received < sizeof(DnsHeader)) return 0;
    auto *header = reinterpret_cast<DnsHeader *>(packet);
    if (ntohs(header->question_count) != 1U || (ntohs(header->flags) & 0x7800U) != 0U) return 0;

    std::size_t cursor = sizeof(DnsHeader);
    while (cursor < received && packet[cursor] != 0U) {
        const std::size_t label_length = packet[cursor];
        if (label_length > 63U || cursor + label_length + 1U >= received) return 0;
        cursor += label_length + 1U;
    }
    if (cursor + 5U > received) return 0;
    ++cursor;
    uint16_t query_type = 0;
    uint16_t query_class = 0;
    std::memcpy(&query_type, packet + cursor, sizeof(query_type));
    std::memcpy(&query_class, packet + cursor + 2U, sizeof(query_class));
    if (ntohs(query_type) != 1U || ntohs(query_class) != 1U) return 0;
    const std::size_t question_end = cursor + 4U;
    if (question_end + sizeof(DnsAnswer) > kPacketBytes) return 0;

    esp_netif_t *access_point = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    esp_netif_ip_info_t ip_info{};
    if (access_point == nullptr || esp_netif_get_ip_info(access_point, &ip_info) != ESP_OK ||
        ip_info.ip.addr == 0U) {
        return 0;
    }

    header->flags = htons(0x8180U);
    header->answer_count = htons(1U);
    header->authority_count = 0;
    header->additional_count = 0;
    DnsAnswer answer{};
    answer.name_pointer = htons(0xC00CU);
    answer.type = htons(1U);
    answer.dns_class = htons(1U);
    answer.ttl = htonl(kAnswerTtlSeconds);
    answer.address_length = htons(4U);
    answer.address = ip_info.ip.addr;
    // Discard any EDNS/additional records from the request. The response header
    // declares none, so the answer must immediately follow the question.
    std::memcpy(packet + question_end, &answer, sizeof(answer));
    return question_end + sizeof(answer);
}

void dns_task(void *)
{
    const int socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socket_handle < 0) {
        ESP_LOGE(kTag, "Could not create UDP socket: errno=%d", errno);
        g_started.store(false);
        vTaskDelete(nullptr);
        return;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kDnsPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(socket_handle, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ESP_LOGE(kTag, "Could not bind UDP/53: errno=%d", errno);
        close(socket_handle);
        g_started.store(false);
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(kTag, "Wildcard captive DNS listening on UDP/53");

    uint8_t packet[kPacketBytes]{};
    while (true) {
        sockaddr_storage source{};
        socklen_t source_length = sizeof(source);
        const int received = recvfrom(socket_handle, packet, sizeof(packet), 0,
                                      reinterpret_cast<sockaddr *>(&source), &source_length);
        if (received <= 0) continue;
        const std::size_t reply_length = make_reply(packet, static_cast<std::size_t>(received));
        if (reply_length == 0U) continue;
        sendto(socket_handle, packet, reply_length, 0,
               reinterpret_cast<sockaddr *>(&source), source_length);
    }
}

void mdns_task(void *)
{
    int socket_handle = -1;
    // esp_wifi_start() returns before the hosted radio has necessarily raised
    // WIFI_EVENT_AP_START/STA_GOT_IP. Joining an IPv4 multicast group during
    // that short window fails, so keep this low-priority task alive and retry.
    while (socket_handle < 0) {
        uint32_t interface_address = 0;
        if (!active_wifi_address(interface_address)) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        socket_handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (socket_handle < 0) {
            ESP_LOGW(kTag, "Waiting to create mDNS socket: errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int reuse = 1;
        setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(kMdnsPort);
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        ip_mreq group{};
        group.imr_multiaddr.s_addr = inet_addr(kMdnsGroup);
        group.imr_interface.s_addr = interface_address;
        if (bind(socket_handle, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
            setsockopt(socket_handle, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &group, sizeof(group)) != 0) {
            ESP_LOGW(kTag, "Waiting to join mDNS group: errno=%d", errno);
            close(socket_handle);
            socket_handle = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    uint8_t ttl = 255;
    setsockopt(socket_handle, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    ESP_LOGI(kTag, "mDNS hostname active: http://%s/", kLocalHostname);

    uint8_t query[kPacketBytes]{};
    uint8_t reply[192]{};
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_port = htons(kMdnsPort);
    destination.sin_addr.s_addr = inet_addr(kMdnsGroup);
    while (true) {
        const int received = recvfrom(socket_handle, query, sizeof(query), 0, nullptr, nullptr);
        if (received <= 0) continue;
        const std::size_t reply_length = make_mdns_reply(
            query, static_cast<std::size_t>(received), reply, sizeof(reply));
        if (reply_length == 0U) continue;
        sendto(socket_handle, reply, reply_length, 0,
               reinterpret_cast<const sockaddr *>(&destination), sizeof(destination));
    }
}

}  // namespace

bool start()
{
    if (g_started.exchange(true)) return true;
    if (xTaskCreate(dns_task, "captive_dns", 4096, nullptr, 4, nullptr) != pdPASS) {
        g_started.store(false);
        ESP_LOGE(kTag, "Could not create captive DNS task");
        return false;
    }
    return true;
}

bool start_mdns()
{
    if (g_mdns_started.exchange(true)) return true;
    if (xTaskCreate(mdns_task, "local_mdns", 3072, nullptr, 3, nullptr) != pdPASS) {
        g_mdns_started.store(false);
        ESP_LOGE(kTag, "Could not create mDNS task");
        return false;
    }
    return true;
}

}  // namespace bluepaws::captive_dns
