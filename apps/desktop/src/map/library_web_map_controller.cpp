#include "map/library_web_map_controller.hpp"

#include "amap_coordinate_transform.hpp"
#include "map_provider_preferences.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QVariant>

#include <algorithm>
#include <cmath>

namespace {

[[nodiscard]] QString compactJson(const QJsonValue& value) {
    if (value.isArray()) {
        return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    }
    return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
}

[[nodiscard]] bool finiteCoordinate(const double latitude, const double longitude) {
    return std::isfinite(latitude) && std::isfinite(longitude) && latitude >= -90.0
           && latitude <= 90.0 && longitude >= -180.0 && longitude <= 180.0;
}

constexpr auto WEB_MAP_DOCUMENT = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no">
<style>
html,body,#map{height:100%;width:100%;margin:0;overflow:hidden;background:#eef1f4;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif}
.shadow-marker{min-width:30px;height:30px;padding:0 8px;border:1px solid #0876bd;border-radius:15px;background:#fff;color:#0876bd;box-sizing:border-box;font:600 12px/28px -apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;text-align:center;box-shadow:0 1px 4px rgba(0,0,0,.24);cursor:pointer}
.shadow-marker.cluster{background:#0876bd;color:#fff;border-color:#005e98}
</style></head><body><div id="map"></div><script>
const config=__SHADOW_CONFIG__;
let map=null,markers=[],pendingMarker=null,state={clusters:[],placement:false,pending:null,center:{latitude:20,longitude:0},zoom:2.5};
let events=[],viewportTimer=0;
function emitEvent(value){events.push(value);if(events.length>64)events.shift()}
window.shadowMapDrainEvents=()=>events.splice(0,64);
function scheduleViewport(){clearTimeout(viewportTimer);viewportTimer=setTimeout(emitViewport,140)}
function markerHtml(cluster){const count=Number(cluster.photoCount||0);return '<div class="shadow-marker '+(count>1?'cluster':'')+'">'+(count>1?count:'&#8226;')+'</div>'}
function clearMarkers(){for(const marker of markers){if(config.provider==='google')marker.setMap(null);else map.remove(marker)}markers=[]}
function googlePoint(value){return {lat:Number(value.latitude),lng:Number(value.longitude)}}
function amapPoint(value){return [Number(value.longitude),Number(value.latitude)]}
function renderMarkers(){if(!map)return;clearMarkers();state.clusters.forEach((cluster,index)=>{
  let marker;
  if(config.provider==='google'){
    marker=new google.maps.Marker({map,position:googlePoint(cluster),title:String(cluster.title||''),label:{text:Number(cluster.photoCount)>1?String(cluster.photoCount):'•',color:Number(cluster.photoCount)>1?'#fff':'#0876bd',fontWeight:'600'},icon:{path:google.maps.SymbolPath.CIRCLE,scale:15,fillColor:Number(cluster.photoCount)>1?'#0876bd':'#fff',fillOpacity:1,strokeColor:'#0876bd',strokeWeight:1}});
    marker.addListener('click',()=>activateCluster(index));
  }else{
    marker=new AMap.Marker({map,position:amapPoint(cluster),anchor:'center',content:markerHtml(cluster),title:String(cluster.title||'')});
    marker.on('click',()=>activateCluster(index));
  }
  markers.push(marker);
});renderPending()}
function activateCluster(index){const cluster=state.clusters[index];if(!cluster||state.placement)return;if(Number(cluster.photoCount)>1){setCenter(cluster.latitude,cluster.longitude,Math.min(20,currentZoom()+2));return}emitEvent({kind:'cluster',index})}
function currentZoom(){return map?Number(map.getZoom()):Number(state.zoom)}
function setCenter(latitude,longitude,zoom){if(!map)return;if(config.provider==='google'){map.setCenter({lat:Number(latitude),lng:Number(longitude)});if(zoom>=0)map.setZoom(Number(zoom))}else{map.setCenter([Number(longitude),Number(latitude)]);if(zoom>=0)map.setZoom(Number(zoom))}}
function renderPending(){if(!map)return;if(pendingMarker){if(config.provider==='google')pendingMarker.setMap(null);else map.remove(pendingMarker);pendingMarker=null}if(!state.pending)return;
 if(config.provider==='google')pendingMarker=new google.maps.Marker({map,position:googlePoint(state.pending),zIndex:10000,icon:{path:google.maps.SymbolPath.BACKWARD_CLOSED_ARROW,scale:7,fillColor:'#0876bd',fillOpacity:1,strokeColor:'#fff',strokeWeight:2}});
 else pendingMarker=new AMap.Marker({map,position:amapPoint(state.pending),anchor:'bottom-center',zIndex:10000,content:'<div style="width:22px;height:22px;border-radius:50% 50% 50% 0;transform:rotate(-45deg);background:#0876bd;border:2px solid white;box-shadow:0 1px 4px rgba(0,0,0,.3)"></div>'});
}
window.shadowMapApplyState=function(next){state=next||state;if(!map)return;setCenter(state.center.latitude,state.center.longitude,state.zoom);renderMarkers();document.getElementById('map').style.cursor=state.placement?'crosshair':''};
function emitViewport(){if(!map)return;let south,west,north,east,center;if(config.provider==='google'){const b=map.getBounds();if(!b)return;const sw=b.getSouthWest(),ne=b.getNorthEast(),c=map.getCenter();south=sw.lat();west=sw.lng();north=ne.lat();east=ne.lng();center={latitude:c.lat(),longitude:c.lng()}}else{const b=map.getBounds(),sw=b.getSouthWest(),ne=b.getNorthEast(),c=map.getCenter();south=sw.getLat();west=sw.getLng();north=ne.getLat();east=ne.getLng();center={latitude:c.getLat(),longitude:c.getLng()}}emitEvent({kind:'viewport',south,west,north,east,center,zoom:currentZoom()})}
function mapClicked(latitude,longitude){if(state.placement)emitEvent({kind:'placement',latitude,longitude})}
function shadowGoogleReady(){map=new google.maps.Map(document.getElementById('map'),{center:googlePoint(state.center),zoom:Number(state.zoom),mapTypeId:config.style,streetViewControl:false,fullscreenControl:false,mapTypeControl:true,clickableIcons:false});map.addListener('idle',scheduleViewport);map.addListener('click',event=>mapClicked(event.latLng.lat(),event.latLng.lng()));renderMarkers();emitEvent({kind:'ready'});scheduleViewport()}
function shadowAmapReady(){const layers=config.style==='satellite'?[new AMap.TileLayer.Satellite(),new AMap.TileLayer.RoadNet()]:undefined;map=new AMap.Map('map',{center:amapPoint(state.center),zoom:Number(state.zoom),layers,viewMode:'2D'});map.on('moveend',scheduleViewport);map.on('zoomend',scheduleViewport);map.on('click',event=>mapClicked(event.lnglat.getLat(),event.lnglat.getLng()));renderMarkers();emitEvent({kind:'ready'});scheduleViewport()}
window.gm_authFailure=()=>emitEvent({kind:'error',code:'authorization-failed'});
function loadProvider(){const script=document.createElement('script');script.async=true;script.onerror=()=>emitEvent({kind:'error',code:'provider-unavailable'});if(config.provider==='google'){window.shadowGoogleReady=shadowGoogleReady;script.src='https://maps.googleapis.com/maps/api/js?key='+encodeURIComponent(config.apiKey)+'&v=weekly&loading=async&callback=shadowGoogleReady&language='+encodeURIComponent(config.language)}else{window._AMapSecurityConfig={securityJsCode:config.securityCode};window.shadowAmapReady=shadowAmapReady;script.src='https://webapi.amap.com/maps?v=2.0&key='+encodeURIComponent(config.apiKey)+'&callback=shadowAmapReady'}document.head.appendChild(script)}
loadProvider();
</script></body></html>)HTML";

} // namespace

LibraryWebMapController::LibraryWebMapController(
    MapProviderPreferences* const preferences,
    QObject* const parent
) : QObject(parent), preferences_(preferences) {
    Q_ASSERT(preferences_ != nullptr);
    connect(
        preferences_,
        &MapProviderPreferences::libraryMapProviderChanged,
        this,
        &LibraryWebMapController::synchronizeProvider
    );
    connect(
        preferences_,
        &MapProviderPreferences::mapStyleChanged,
        this,
        &LibraryWebMapController::synchronizeProvider
    );
    connect(
        preferences_,
        &MapProviderPreferences::googleApiKeyStoredChanged,
        this,
        &LibraryWebMapController::synchronizeProvider
    );
    connect(
        preferences_,
        &MapProviderPreferences::amapJsCredentialsStoredChanged,
        this,
        &LibraryWebMapController::synchronizeProvider
    );
}

bool LibraryWebMapController::active() const noexcept { return active_; }
QString LibraryWebMapController::providerId() const { return preferences_->libraryMapProvider(); }
bool LibraryWebMapController::providerSelected() const { return providerId() != QStringLiteral("none"); }
bool LibraryWebMapController::providerAvailable() const {
    return (providerId() == QStringLiteral("google") && preferences_->googleApiKeyStored())
           || (providerId() == QStringLiteral("amap") && preferences_->amapJsCredentialsStored());
}
QString LibraryWebMapController::providerName() const {
    if (providerId() == QStringLiteral("google")) return QStringLiteral("Google Maps");
    if (providerId() == QStringLiteral("amap")) return QStringLiteral("AMap");
    return {};
}
bool LibraryWebMapController::ready() const noexcept { return ready_; }
bool LibraryWebMapController::busy() const noexcept { return busy_; }
QString LibraryWebMapController::statusCode() const { return status_code_; }
double LibraryWebMapController::centerLatitude() const noexcept { return center_latitude_; }
double LibraryWebMapController::centerLongitude() const noexcept { return center_longitude_; }
double LibraryWebMapController::zoomLevel() const noexcept { return zoom_level_; }

void LibraryWebMapController::attachWebView(QObject* const web_view) {
    if (web_view_ == web_view) return;
    web_view_ = web_view;
    reloadDocument();
}

void LibraryWebMapController::detachWebView(QObject* const web_view) {
    if (web_view_ != web_view) return;
    web_view_.clear();
    setRuntimeState(false, false);
}

void LibraryWebMapController::setActive(const bool active) {
    if (active_ == active) return;
    active_ = active;
    if (active_) {
        reloadDocument();
    } else {
        setRuntimeState(false, false);
    }
    emit stateChanged();
}

void LibraryWebMapController::setLanguage(const QString& language) {
    const QString normalized = language.startsWith(QStringLiteral("zh"), Qt::CaseInsensitive)
                                   ? QStringLiteral("zh-CN")
                                   : QStringLiteral("en-US");
    if (language_ == normalized) return;
    language_ = normalized;
    reloadDocument();
}

void LibraryWebMapController::setClusters(const QVariantList& clusters) {
    clusters_ = clusters;
    pushState();
}

void LibraryWebMapController::setCenter(
    const double latitude,
    const double longitude,
    const double zoom
) {
    if (!finiteCoordinate(latitude, longitude)) return;
    center_latitude_ = latitude;
    center_longitude_ = longitude;
    if (std::isfinite(zoom) && zoom >= 0.0) zoom_level_ = std::clamp(zoom, 1.0, 20.0);
    emit centerChanged();
    pushState();
}

void LibraryWebMapController::setPlacementActive(const bool active) {
    if (placement_active_ == active) return;
    placement_active_ = active;
    pushState();
}

void LibraryWebMapController::setPendingCoordinate(
    const bool present,
    const double latitude,
    const double longitude
) {
    pending_coordinate_present_ = present && finiteCoordinate(latitude, longitude);
    pending_latitude_ = latitude;
    pending_longitude_ = longitude;
    pushState();
}

void LibraryWebMapController::consumeEvents(const QString& json) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) return;
    for (const QJsonValue& value : document.array()) {
        const QJsonObject event = value.toObject();
        const QString kind = event.value(QStringLiteral("kind")).toString();
        if (kind == QStringLiteral("ready")) {
            setRuntimeState(true, false);
            pushState();
            continue;
        }
        if (kind == QStringLiteral("error")) {
            setRuntimeState(false, false, event.value(QStringLiteral("code")).toString());
            continue;
        }
        if (kind == QStringLiteral("cluster")) {
            const int index = event.value(QStringLiteral("index")).toInt(-1);
            if (index >= 0 && index < clusters_.size()) emit clusterActivated(clusters_.at(index).toMap());
            continue;
        }
        if (kind == QStringLiteral("placement")) {
            double latitude = event.value(QStringLiteral("latitude")).toDouble();
            double longitude = event.value(QStringLiteral("longitude")).toDouble();
            if (providerId() == QStringLiteral("amap")) {
                const auto converted = shadow::desktop::maps::gcj02ToWgs84(latitude, longitude);
                latitude = converted.latitude;
                longitude = converted.longitude;
            }
            if (finiteCoordinate(latitude, longitude)) emit coordinateProposed(latitude, longitude);
            continue;
        }
        if (kind != QStringLiteral("viewport")) continue;
        double south = event.value(QStringLiteral("south")).toDouble();
        double west = event.value(QStringLiteral("west")).toDouble();
        double north = event.value(QStringLiteral("north")).toDouble();
        double east = event.value(QStringLiteral("east")).toDouble();
        double center_latitude = event.value(QStringLiteral("center")).toObject().value(QStringLiteral("latitude")).toDouble();
        double center_longitude = event.value(QStringLiteral("center")).toObject().value(QStringLiteral("longitude")).toDouble();
        if (providerId() == QStringLiteral("amap")) {
            const auto south_west = shadow::desktop::maps::gcj02ToWgs84(south, west);
            const auto north_east = shadow::desktop::maps::gcj02ToWgs84(north, east);
            const auto center = shadow::desktop::maps::gcj02ToWgs84(center_latitude, center_longitude);
            south = south_west.latitude; west = south_west.longitude;
            north = north_east.latitude; east = north_east.longitude;
            center_latitude = center.latitude; center_longitude = center.longitude;
        }
        const int zoom = std::clamp(event.value(QStringLiteral("zoom")).toInt(2), 1, 20);
        if (finiteCoordinate(center_latitude, center_longitude)) {
            center_latitude_ = center_latitude;
            center_longitude_ = center_longitude;
            zoom_level_ = zoom;
            emit centerChanged();
        }
        if (finiteCoordinate(south, west) && finiteCoordinate(north, east) && south < north) {
            emit viewportChanged(south, west, north, east, zoom);
        }
    }
}

void LibraryWebMapController::handleLoadStatus(const int status, const QString& error_text) {
    if (status == 0) setRuntimeState(false, true);
    else if (status == 2) setRuntimeState(false, true);
    else if (status == 3) setRuntimeState(false, false, error_text.isEmpty() ? QStringLiteral("load-failed") : QStringLiteral("load-failed"));
}

QString LibraryWebMapController::buildDocument() {
    QJsonObject config{
        {QStringLiteral("provider"), providerId()},
        {QStringLiteral("language"), language_},
        {QStringLiteral("style"), preferences_->mapStyle()},
    };
    if (providerId() == QStringLiteral("google")) {
        const SecretStoreResult secret = preferences_->readGoogleApiKey();
        if (!secret.succeeded()) { status_code_ = QStringLiteral("credential-unavailable"); return {}; }
        config.insert(QStringLiteral("apiKey"), secret.value);
    } else if (providerId() == QStringLiteral("amap")) {
        const AmapJsCredentialsResult credentials = preferences_->readAmapJsCredentials();
        if (!credentials.succeeded()) { status_code_ = QStringLiteral("credential-unavailable"); return {}; }
        config.insert(QStringLiteral("apiKey"), credentials.api_key);
        config.insert(QStringLiteral("securityCode"), credentials.security_code);
    } else {
        return {};
    }
    QString document = QString::fromUtf8(WEB_MAP_DOCUMENT);
    return document.replace(QStringLiteral("__SHADOW_CONFIG__"), compactJson(config));
}

QVariantMap LibraryWebMapController::providerCoordinate(
    const double latitude,
    const double longitude
) const {
    if (providerId() == QStringLiteral("amap")) {
        const auto converted = shadow::desktop::maps::wgs84ToGcj02(latitude, longitude);
        return {{QStringLiteral("latitude"), converted.latitude}, {QStringLiteral("longitude"), converted.longitude}};
    }
    return {{QStringLiteral("latitude"), latitude}, {QStringLiteral("longitude"), longitude}};
}

QVariantMap LibraryWebMapController::presentationState() const {
    QVariantList presented_clusters;
    presented_clusters.reserve(clusters_.size());
    for (const QVariant& value : clusters_) {
        QVariantMap cluster = value.toMap();
        const QVariantMap coordinate = providerCoordinate(cluster.value(QStringLiteral("latitude")).toDouble(), cluster.value(QStringLiteral("longitude")).toDouble());
        cluster.insert(QStringLiteral("latitude"), coordinate.value(QStringLiteral("latitude")));
        cluster.insert(QStringLiteral("longitude"), coordinate.value(QStringLiteral("longitude")));
        presented_clusters.push_back(cluster);
    }
    QVariantMap state{
        {QStringLiteral("clusters"), presented_clusters},
        {QStringLiteral("placement"), placement_active_},
        {QStringLiteral("center"), providerCoordinate(center_latitude_, center_longitude_)},
        {QStringLiteral("zoom"), zoom_level_},
    };
    if (pending_coordinate_present_) state.insert(QStringLiteral("pending"), providerCoordinate(pending_latitude_, pending_longitude_));
    else state.insert(QStringLiteral("pending"), QVariant{});
    return state;
}

void LibraryWebMapController::reloadDocument() {
    if (!active_ || web_view_.isNull() || !providerAvailable()) {
        setRuntimeState(false, false);
        return;
    }
    const QString document = buildDocument();
    if (document.isEmpty()) { emit stateChanged(); return; }
    setRuntimeState(false, true);
    QMetaObject::invokeMethod(web_view_, "loadHtml", Q_ARG(QString, document));
}

void LibraryWebMapController::pushState() {
    if (!ready_ || web_view_.isNull()) return;
    const QString json = compactJson(QJsonObject::fromVariantMap(presentationState()));
    runJavaScript(QStringLiteral("window.shadowMapApplyState(%1)").arg(json));
}

void LibraryWebMapController::runJavaScript(const QString& script) {
    if (!web_view_.isNull()) QMetaObject::invokeMethod(web_view_, "runJavaScript", Q_ARG(QString, script));
}

void LibraryWebMapController::setRuntimeState(
    const bool ready,
    const bool busy,
    const QString& status_code
) {
    if (ready_ == ready && busy_ == busy && status_code_ == status_code) return;
    ready_ = ready;
    busy_ = busy;
    status_code_ = status_code;
    emit stateChanged();
}

void LibraryWebMapController::synchronizeProvider() {
    emit stateChanged();
    reloadDocument();
}
