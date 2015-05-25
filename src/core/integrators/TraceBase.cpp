#include "TraceBase.hpp"

namespace Tungsten {

TraceBase::TraceBase(TraceableScene *scene, const TraceSettings &settings, uint32 threadId)
: _scene(scene),
  _settings(settings),
  _threadId(threadId)
{
    _scene = scene;
    _lightPdf.resize(scene->lights().size());

    std::vector<float> lightWeights(scene->lights().size());
    for (size_t i = 0; i < scene->lights().size(); ++i) {
        scene->lights()[i]->makeSamplable(_threadId);
        lightWeights[i] = 1.0f; // TODO: Use light power here
    }
    _lightSampler.reset(new Distribution1D(std::move(lightWeights)));

    for (const auto &prim : scene->lights())
        prim->makeSamplable(_threadId);
}

SurfaceScatterEvent TraceBase::makeLocalScatterEvent(IntersectionTemporary &data, IntersectionInfo &info,
        Ray &ray, SampleGenerator *sampler, SampleGenerator *supplementalSampler) const
{
    TangentFrame frame;
    info.primitive->setupTangentFrame(data, info, frame);

    bool hitBackside = frame.normal.dot(ray.dir()) > 0.0f;
    bool isTransmissive = info.bsdf->lobes().isTransmissive();

    bool flipFrame = _settings.enableTwoSidedShading && hitBackside && !isTransmissive;

    if (flipFrame) {
        // TODO: Should we flip info.Ns here too? It doesn't seem to be used at the moment,
        // but it may be in the future. Modifying the intersection info itself could be a bad
        // idea though
        frame.normal = -frame.normal;
        frame.tangent = -frame.tangent;
    }

    return SurfaceScatterEvent(
        &info,
        sampler,
        supplementalSampler,
        frame,
        frame.toLocal(-ray.dir()),
        BsdfLobes::AllLobes,
        flipFrame
    );
}

bool TraceBase::isConsistent(const SurfaceScatterEvent &event, const Vec3f &w) const
{
    if (!_settings.enableConsistencyChecks)
        return true;
    bool geometricBackside = (w.dot(event.info->Ng) < 0.0f);
    bool shadingBackside = (event.wo.z() < 0.0f) ^ event.flippedFrame;
    return geometricBackside == shadingBackside;
}

Vec3f TraceBase::generalizedShadowRay(Ray &ray,
                           const Medium *medium,
                           const Primitive *endCap,
                           int bounce)
{
    IntersectionTemporary data;
    IntersectionInfo info;

    float initialFarT = ray.farT();
    Vec3f throughput(1.0f);
    do {
        if (_scene->intersect(ray, data, info) && info.primitive != endCap) {
            SurfaceScatterEvent event = makeLocalScatterEvent(data, info, ray, nullptr, nullptr);

            // For forward events, the transport direction does not matter (since wi = -wo)
            Vec3f transmittance = info.bsdf->eval(event.makeForwardEvent(), false);
            if (transmittance == 0.0f)
                return Vec3f(0.0f);

            throughput *= transmittance;
            bounce++;

            if (bounce >= _settings.maxBounces)
                return Vec3f(0.0f);
        }

        if (medium)
            throughput *= medium->transmittance(VolumeScatterEvent(ray.pos(), ray.dir(), ray.farT()));
        if (info.primitive == nullptr || info.primitive == endCap)
            return bounce >= _settings.minBounces ? throughput : Vec3f(0.0f);
        medium = info.bsdf->selectMedium(medium, !info.primitive->hitBackside(data));

        ray.setPos(ray.hitpoint());
        initialFarT -= ray.farT();
        ray.setNearT(info.epsilon);
        ray.setFarT(initialFarT);
    } while(true);
    return Vec3f(0.0f);
}

Vec3f TraceBase::attenuatedEmission(const Primitive &light,
                         const Medium *medium,
                         float expectedDist,
                         IntersectionTemporary &data,
                         IntersectionInfo &info,
                         int bounce,
                         Ray &ray)
{
    CONSTEXPR float fudgeFactor = 1.0f + 1e-3f;

    if (light.isDirac()) {
        ray.setFarT(expectedDist);
    } else {
        if (!light.intersect(ray, data) || ray.farT()*fudgeFactor < expectedDist)
            return Vec3f(0.0f);
    }
    info.p = ray.pos() + ray.dir()*ray.farT();
    info.w = ray.dir();
    light.intersectionInfo(data, info);

    Vec3f transmittance = generalizedShadowRay(ray, medium, &light, bounce);
    if (transmittance == 0.0f)
        return Vec3f(0.0f);

    return transmittance*light.emission(data, info);
}

bool TraceBase::lensSample(const Camera &camera,
                           SurfaceScatterEvent &event,
                           const Medium *medium,
                           int bounce,
                           const Ray &parentRay,
                           Vec3f &weight,
                           Vec2f &pixel)
{
    LensSample sample;
    if (!camera.sampleDirect(event.info->p, *event.sampler, sample))
        return false;

    event.wo = event.frame.toLocal(sample.d);
    if (!isConsistent(event, sample.d))
        return false;

    bool geometricBackside = (sample.d.dot(event.info->Ng) < 0.0f);
    medium = event.info->bsdf->selectMedium(medium, geometricBackside);

    event.requestedLobe = BsdfLobes::AllButSpecular;

    Vec3f f = event.info->bsdf->eval(event, true);
    if (f == 0.0f)
        return false;

    Ray ray = parentRay.scatter(event.info->p, sample.d, event.info->epsilon);
    ray.setPrimaryRay(false);
    ray.setFarT(sample.dist);

    IntersectionTemporary data;
    IntersectionInfo info;
    Vec3f transmittance = generalizedShadowRay(ray, medium, nullptr, bounce);
    if (transmittance == 0.0f)
        return false;

    weight = f*transmittance*sample.weight;
    pixel = sample.pixel;

    return true;
}

Vec3f TraceBase::lightSample(const Primitive &light,
                             SurfaceScatterEvent &event,
                             const Medium *medium,
                             int bounce,
                             const Ray &parentRay)
{
    LightSample sample(event.sampler, event.info->p);

    if (!light.sampleInboundDirection(_threadId, sample))
        return Vec3f(0.0f);

    event.wo = event.frame.toLocal(sample.d);
    if (!isConsistent(event, sample.d))
        return Vec3f(0.0f);

    bool geometricBackside = (sample.d.dot(event.info->Ng) < 0.0f);
    medium = event.info->bsdf->selectMedium(medium, geometricBackside);

    event.requestedLobe = BsdfLobes::AllButSpecular;

    Vec3f f = event.info->bsdf->eval(event, false);
    if (f == 0.0f)
        return Vec3f(0.0f);

    Ray ray = parentRay.scatter(sample.p, sample.d, event.info->epsilon);
    ray.setPrimaryRay(false);

    IntersectionTemporary data;
    IntersectionInfo info;
    Vec3f e = attenuatedEmission(light, medium, sample.dist, data, info, bounce, ray);
    if (e == 0.0f)
        return Vec3f(0.0f);

    Vec3f lightF = f*e/sample.pdf;

    if (!light.isDirac())
        lightF *= SampleWarp::powerHeuristic(sample.pdf, event.info->bsdf->pdf(event));

    return lightF;
}

Vec3f TraceBase::bsdfSample(const Primitive &light,
                            SurfaceScatterEvent &event,
                            const Medium *medium,
                            int bounce,
                            const Ray &parentRay)
{
    event.requestedLobe = BsdfLobes::AllButSpecular;
    if (!event.info->bsdf->sample(event, false))
        return Vec3f(0.0f);
    if (event.throughput == 0.0f)
        return Vec3f(0.0f);

    Vec3f wo = event.frame.toGlobal(event.wo);
    if (!isConsistent(event, wo))
        return Vec3f(0.0f);

    bool geometricBackside = (wo.dot(event.info->Ng) < 0.0f);
    medium = event.info->bsdf->selectMedium(medium, geometricBackside);

    Ray ray = parentRay.scatter(event.info->p, wo, event.info->epsilon);
    ray.setPrimaryRay(false);

    IntersectionTemporary data;
    IntersectionInfo info;
    Vec3f e = attenuatedEmission(light, medium, -1.0f, data, info, bounce, ray);

    if (e == Vec3f(0.0f))
        return Vec3f(0.0f);

    Vec3f bsdfF = e*event.throughput;

    bsdfF *= SampleWarp::powerHeuristic(event.pdf, light.inboundPdf(_threadId, data, info, event.info->p, wo));

    return bsdfF;
}

Vec3f TraceBase::volumeLightSample(VolumeScatterEvent &event,
                    const Primitive &light,
                    const Medium *medium,
                    bool performMis,
                    int bounce,
                    const Ray &parentRay)
{
    LightSample sample(event.sampler, event.p);

    if (!light.sampleInboundDirection(_threadId, sample))
        return Vec3f(0.0f);
    event.wo = sample.d;

    Vec3f f = medium->phaseEval(event);
    if (f == 0.0f)
        return Vec3f(0.0f);

    Ray ray = parentRay.scatter(sample.p, sample.d, 0.0f);
    ray.setPrimaryRay(false);

    IntersectionTemporary data;
    IntersectionInfo info;
    Vec3f e = attenuatedEmission(light, medium, sample.dist, data, info, bounce, ray);
    if (e == 0.0f)
        return Vec3f(0.0f);

    Vec3f lightF = f*e/sample.pdf;

    if (!light.isDirac() && performMis)
        lightF *= SampleWarp::powerHeuristic(sample.pdf, medium->phasePdf(event));

    return lightF;
}

Vec3f TraceBase::volumePhaseSample(const Primitive &light,
                    VolumeScatterEvent &event,
                    const Medium *medium,
                    int bounce,
                    const Ray &parentRay)
{
    if (!medium->scatter(event))
        return Vec3f(0.0f);
    if (event.throughput == 0.0f)
        return Vec3f(0.0f);

    Ray ray = parentRay.scatter(event.p, event.wo, 0.0f);
    ray.setPrimaryRay(false);

    IntersectionTemporary data;
    IntersectionInfo info;
    Vec3f e = attenuatedEmission(light, medium, -1.0f, data, info, bounce, ray);

    if (e == Vec3f(0.0f))
        return Vec3f(0.0f);

    Vec3f phaseF = e*event.throughput;

    phaseF *= SampleWarp::powerHeuristic(event.pdf, light.inboundPdf(_threadId, data, info, event.p, event.wo));

    return phaseF;
}

Vec3f TraceBase::sampleDirect(const Primitive &light,
                              SurfaceScatterEvent &event,
                              const Medium *medium,
                              int bounce,
                              const Ray &parentRay)
{
    Vec3f result(0.0f);

    if (event.info->bsdf->lobes().isPureSpecular() || event.info->bsdf->lobes().isForward())
        return Vec3f(0.0f);

    result += lightSample(light, event, medium, bounce, parentRay);
    if (!light.isDirac())
        result += bsdfSample(light, event, medium, bounce, parentRay);

    return result;
}

Vec3f TraceBase::volumeSampleDirect(const Primitive &light,
                    VolumeScatterEvent &event,
                    const Medium *medium,
                    int bounce,
                    const Ray &parentRay)
{
    // TODO: Re-enable Mis suggestions? Might be faster, but can cause fireflies
    bool mis = true;//medium->suggestMis();

    Vec3f result = volumeLightSample(event, light, medium, mis, bounce, parentRay);
    if (!light.isDirac() && mis)
        result += volumePhaseSample(light, event, medium, bounce, parentRay);

    return result;
}

const Primitive *TraceBase::chooseLight(SampleGenerator &sampler, const Vec3f &p, float &weight)
{
    if (_scene->lights().empty())
        return nullptr;
    if (_scene->lights().size() == 1) {
        weight = 1.0f;
        return _scene->lights()[0].get();
    }

    float total = 0.0f;
    unsigned numNonNegative = 0;
    for (size_t i = 0; i < _lightPdf.size(); ++i) {
        _lightPdf[i] = _scene->lights()[i]->approximateRadiance(_threadId, p);
        if (_lightPdf[i] >= 0.0f) {
            total += _lightPdf[i];
            numNonNegative++;
        }
    }
    if (numNonNegative == 0) {
        for (size_t i = 0; i < _lightPdf.size(); ++i)
            _lightPdf[i] = 1.0f;
        total = _lightPdf.size();
    } else if (numNonNegative < _lightPdf.size()) {
        for (size_t i = 0; i < _lightPdf.size(); ++i) {
            float uniformWeight = (total == 0.0f ? 1.0f : total)/numNonNegative;
            if (_lightPdf[i] < 0.0f) {
                _lightPdf[i] = uniformWeight;
                total += uniformWeight;
            }
        }
    }
    if (total == 0.0f)
        return nullptr;
    float t = sampler.next1D()*total;
    for (size_t i = 0; i < _lightPdf.size(); ++i) {
        if (t < _lightPdf[i] || i == _lightPdf.size() - 1) {
            weight = total/_lightPdf[i];
            return _scene->lights()[i].get();
        } else {
            t -= _lightPdf[i];
        }
    }
    return nullptr;
}

const Primitive *TraceBase::chooseLightAdjoint(SampleGenerator &sampler, float &pdf)
{
    float u = sampler.next1D();
    int lightIdx;
    _lightSampler->warp(u, lightIdx);
    pdf = _lightSampler->pdf(lightIdx);
    return _scene->lights()[lightIdx].get();
}

Vec3f TraceBase::volumeEstimateDirect(VolumeScatterEvent &event,
                    const Medium *medium,
                    int bounce,
                    const Ray &parentRay)
{
    float weight;
    const Primitive *light = chooseLight(*event.sampler, event.p, weight);
    if (light == nullptr)
        return Vec3f(0.0f);
    return volumeSampleDirect(*light, event, medium, bounce, parentRay)*weight;
}

Vec3f TraceBase::estimateDirect(SurfaceScatterEvent &event,
                                const Medium *medium,
                                int bounce,
                                const Ray &parentRay)
{
    float weight;
    const Primitive *light = chooseLight(*event.sampler, event.info->p, weight);
    if (light == nullptr)
        return Vec3f(0.0f);
    return sampleDirect(*light, event, medium, bounce, parentRay)*weight;
}

bool TraceBase::handleVolume(SampleGenerator &sampler, SampleGenerator &supplementalSampler,
           const Medium *&medium, int bounce, bool adjoint, bool enableLightSampling, Ray &ray,
           Vec3f &throughput, Vec3f &emission, bool &wasSpecular, bool &hitSurface,
           Medium::MediumState &state)
{
    VolumeScatterEvent event(&sampler, &supplementalSampler, throughput, ray.pos(), ray.dir(), ray.farT());
    if (!medium->sampleDistance(event, state))
        return false;
    throughput *= event.throughput;
    event.throughput = Vec3f(1.0f);

    if (!adjoint && bounce >= _settings.minBounces)
        emission += throughput*medium->emission(event);

    if (!enableLightSampling)
        wasSpecular = !hitSurface;

    if (event.t < event.maxT) {
        event.p += event.t*event.wi;

        if (!adjoint && enableLightSampling && bounce < _settings.maxBounces - 1) {
            wasSpecular = false;
            emission += throughput*volumeEstimateDirect(event, medium, bounce + 1, ray);
        }

        if (medium->absorb(event, state))
            return false;
        if (!medium->scatter(event))
            return false;
        ray = ray.scatter(event.p, event.wo, 0.0f);
        ray.setPrimaryRay(false);
        throughput *= event.throughput;
        hitSurface = false;
    } else {
        hitSurface = true;
    }

    return true;
}

bool TraceBase::handleSurface(SurfaceScatterEvent &event, IntersectionTemporary &data,
                              IntersectionInfo &info, SampleGenerator &sampler,
                              SampleGenerator &supplementalSampler, const Medium *&medium,
                              int bounce, bool adjoint, bool enableLightSampling, Ray &ray,
                              Vec3f &throughput, Vec3f &emission, bool &wasSpecular,
                              Medium::MediumState &state)
{
    const Bsdf &bsdf = *info.bsdf;

    // For forward events, the transport direction does not matter (since wi = -wo)
    Vec3f transparency = bsdf.eval(event.makeForwardEvent(), false);
    float transparencyScalar = transparency.avg();

    Vec3f wo;
    if (supplementalSampler.next1D() < transparencyScalar) {
        wo = ray.dir();
        throughput *= transparency/transparencyScalar;
    } else {
        if (!adjoint) {
            if (enableLightSampling) {
                if ((wasSpecular || !info.primitive->isSamplable()) && bounce >= _settings.minBounces)
                    emission += info.primitive->emission(data, info)*throughput;

                if (bounce < _settings.maxBounces - 1)
                    emission += estimateDirect(event, medium, bounce + 1, ray)*throughput;
            } else if (bounce >= _settings.minBounces) {
                emission += info.primitive->emission(data, info)*throughput;
            }
        }

        event.requestedLobe = BsdfLobes::AllLobes;
        if (!bsdf.sample(event, adjoint))
            return false;

        wo = event.frame.toGlobal(event.wo);

        if (!isConsistent(event, wo))
            return false;

        throughput *= event.throughput;
        wasSpecular = event.sampledLobe.hasSpecular();
        if (!wasSpecular)
            ray.setPrimaryRay(false);
    }

    bool geometricBackside = (wo.dot(info.Ng) < 0.0f);
    medium = bsdf.selectMedium(medium, geometricBackside);
    state.reset();

    ray = ray.scatter(ray.hitpoint(), wo, info.epsilon);

    return true;
}

}
