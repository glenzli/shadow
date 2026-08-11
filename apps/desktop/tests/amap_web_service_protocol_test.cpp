#include "amap_web_service_protocol.hpp"

#include <cstdlib>
#include <iostream>

namespace {

[[nodiscard]] bool require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "AMap Web Service protocol contract failed: " << message << '\n';
    }
    return condition;
}

} // namespace

int main() {
    using namespace shadow::desktop::maps;

    const AmapReverseGeocodeResponse reverse = parseAmapReverseGeocodeResponse(R"json(
        {
          "status":"1","info":"OK","infocode":"10000",
          "regeocode":{
            "formatted_address":"北京市东城区东华门街道天安门",
            "addressComponent":{
              "country":"中国","province":"北京市","city":[],
              "district":"东城区","adcode":"110101"
            }
          }
        }
    )json");
    if (!require(
            reverse.result && !reverse.failure
                && reverse.result->country_code == QStringLiteral("CN")
                && reverse.result->administrative_area == QStringLiteral("北京市")
                && reverse.result->locality == QStringLiteral("北京市")
                && reverse.result->display_name.contains(QStringLiteral("天安门")),
            "a municipality response becomes a stable country, region, locality, and label"
        )) {
        return EXIT_FAILURE;
    }

    const AmapPlaceSearchResponse places = parseAmapPlaceSearchResponse(R"json(
        {
          "status":"1","info":"OK","infocode":"10000","count":"2",
          "pois":[
            {"id":"B000A83VHF","name":"天安门","address":"东长安街",
             "pname":"北京市","cityname":"北京市","location":"116.397499,39.908722"},
            {"id":"broken","name":"缺少坐标"}
          ]
        }
    )json");
    if (!require(
            !places.failure && places.results.size() == 1
                && places.results.front().id == QStringLiteral("B000A83VHF")
                && places.results.front().name == QStringLiteral("天安门")
                && places.results.front().gcj02_longitude == 116.397499,
            "place search accepts bounded structured POIs and ignores incomplete entries"
        )) {
        return EXIT_FAILURE;
    }

    const AmapReverseGeocodeResponse denied = parseAmapReverseGeocodeResponse(
        R"json({"status":"0","info":"INVALID_USER_KEY","infocode":"10001"})json"
    );
    return require(
               !denied.result && denied.failure
                   && denied.failure->code == QStringLiteral("10001"),
               "provider failures remain structured without exposing credentials"
           )
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}
