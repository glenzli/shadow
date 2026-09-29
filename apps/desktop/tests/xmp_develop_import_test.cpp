#include "xmp_develop_import.hpp"

#include <QCoreApplication>

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

void require(const bool condition, const char* const message) {
    if (!condition) {
        std::cerr << "XMP develop import test failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] bool close_to(const double left, const double right) {
    return std::abs(left - right) < 1e-9;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);

    const auto baked = parseXmpDevelopImport(
        R"(<x xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/" crs:AlreadyApplied="True" crs:Exposure2012="0.25"/>)"
    );
    require(
        baked.already_applied && baked.canApply(),
        "already-applied evidence survives for an explicit warning"
    );
    const auto preview = parseXmpDevelopImport(R"XMP(
        <x:xmpmeta xmlns:x="adobe:ns:meta/">
          <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
            <rdf:Description
              xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"
              crs:ProcessVersion="15.4"
              crs:Exposure2012="+0.75"
              crs:Contrast2012="25"
              crs:Highlights2012="-40"
              crs:Texture="10"
              crs:Saturation="-20"
              crs:Temperature="5250"
              crs:ToneCurveName2012="Custom" />
          </rdf:RDF>
        </x:xmpmeta>
    )XMP");
    require(preview.document_error.isEmpty(), "valid XML parses");
    require(preview.process_version == QStringLiteral("15.4"), "process version is retained");
    require(preview.adjustments.size() == 5, "controlled scalar fields are mapped");
    require(preview.ignored_fields.size() == 2, "non-equivalent fields are reported");
    require(preview.invalid_fields.isEmpty(), "valid mapped fields are accepted");
    require(preview.canApply(), "valid partial preview can apply");
    require(
        preview.adjustments[0].target == XmpDevelopTarget::ExposureStops
            && close_to(preview.adjustments[0].target_value, 0.75),
        "exposure retains stop value"
    );
    require(
        preview.adjustments[1].target == XmpDevelopTarget::ContrastFactor
            && close_to(preview.adjustments[1].target_value, std::pow(2.0, 0.25)),
        "contrast uses the declared bounded approximation"
    );
    require(
        preview.adjustments[4].target == XmpDevelopTarget::SaturationFactor
            && close_to(preview.adjustments[4].target_value, 0.8),
        "saturation maps to the multiplicative chroma control"
    );

    const auto invalid = parseXmpDevelopImport(R"XMP(
        <rdf:Description
          xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
          xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"
          crs:Exposure2012="nan"
          crs:Clarity2012="120" />
    )XMP");
    require(invalid.invalid_fields.size() == 2, "invalid supported values are explicit");
    require(!invalid.canApply(), "invalid supported values block a partial import");

    const auto child_value = parseXmpDevelopImport(R"XMP(
        <rdf:Description
          xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#"
          xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/">
          <crs:Dehaze>30</crs:Dehaze>
        </rdf:Description>
    )XMP");
    require(
        child_value.adjustments.size() == 1
            && close_to(child_value.adjustments.front().target_value, 0.3),
        "element-form scalar values are supported"
    );

    const auto malformed = parseXmpDevelopImport("<x:xmpmeta>");
    require(!malformed.document_error.isEmpty(), "malformed XML fails closed");
    require(!malformed.canApply(), "malformed XML cannot apply");

    const auto mixer = parseXmpDevelopImport(R"XMP(
      <x xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"
         crs:HueAdjustmentOrange="-35" crs:SaturationAdjustmentBlue="50"
         crs:LuminanceAdjustmentMagenta="-20" crs:ColorGradeMidtoneHue="180">
         <crs:ToneCurvePV2012><rdf:Seq xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
           <rdf:li>0, 0</rdf:li><rdf:li>255, 255</rdf:li>
         </rdf:Seq></crs:ToneCurvePV2012>
      </x>)XMP");
    require(mixer.canApply() && mixer.adjustments.size() == 3 && mixer.ignored_fields.size() == 2,
        "HSL is mapped while non-equivalent curves and color wheels remain explicit");
    require(mixer.adjustments[0].target == XmpDevelopTarget::MixerHue
            && mixer.adjustments[0].color_band == 1
            && close_to(mixer.adjustments[0].target_value, -0.35),
        "orange hue was mapped to the wrong band or amount");
    require(mixer.adjustments[1].color_band == 5
            && mixer.adjustments[2].color_band == 7,
        "blue and magenta mapping lost band identity");
    const auto conflicting = parseXmpDevelopImport(R"XMP(
      <x xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/" crs:HueAdjustmentRed="5">
        <crs:HueAdjustmentRed>6</crs:HueAdjustmentRed>
      </x>)XMP");
    require(!conflicting.canApply() && conflicting.invalid_fields.size() == 1,
        "conflicting HSL values did not block import");

    std::cout << "XMP develop import contract passed\n";
    return EXIT_SUCCESS;
}
