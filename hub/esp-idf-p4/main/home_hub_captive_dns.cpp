#include "home_hub_captive_dns.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bluepaws::captive_dns {
namespace {

constexpr char kTag[] = "hub_captive_dns";
constexpr uint16_t kDnsPort = 53;
constexpr std::size_t kPacketBytes = 512;
constexpr uint32_t kAnswerTtlSeconds = 60;

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

}  // namespace bluepaws::captive_dns
