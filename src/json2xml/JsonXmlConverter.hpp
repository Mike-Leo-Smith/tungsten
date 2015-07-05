#ifndef JSONXMLCONVERTER_HPP_
#define JSONXMLCONVERTER_HPP_

#include "primitives/InfiniteSphere.hpp"
#include "primitives/TriangleMesh.hpp"
#include "primitives/Sphere.hpp"
#include "primitives/Quad.hpp"

#include "materials/ConstantTexture.hpp"
#include "materials/CheckerTexture.hpp"
#include "materials/BitmapTexture.hpp"

#include "cameras/ThinlensCamera.hpp"
#include "cameras/PinholeCamera.hpp"

#include "volume/HomogeneousMedium.hpp"
#include "volume/Medium.hpp"

#include "bsdfs/RoughDielectricBsdf.hpp"
#include "bsdfs/RoughConductorBsdf.hpp"
#include "bsdfs/TransparencyBsdf.hpp"
#include "bsdfs/DielectricBsdf.hpp"
#include "bsdfs/SmoothCoatBsdf.hpp"
#include "bsdfs/ConductorBsdf.hpp"
#include "bsdfs/OrenNayarBsdf.hpp"
#include "bsdfs/ThinSheetBsdf.hpp"
#include "bsdfs/LambertBsdf.hpp"
#include "bsdfs/ForwardBsdf.hpp"
#include "bsdfs/PlasticBsdf.hpp"
#include "bsdfs/MirrorBsdf.hpp"
#include "bsdfs/NullBsdf.hpp"
#include "bsdfs/PhongBsdf.hpp"
#include "bsdfs/MixedBsdf.hpp"
#include "bsdfs/Bsdf.hpp"

#include "io/FileUtils.hpp"
#include "io/Scene.hpp"

#include "Debug.hpp"

#include <iostream>
#include <string>
#include <stack>

namespace Tungsten {

class SceneXmlWriter
{
    Path _folder;
    std::ostream &_stream;
    std::string _indent;
    std::stack<std::string> _blocks;
    Scene &_scene;

    void begin(const std::string &block)
    {
        _stream << _indent << "<" << block << " ";
        _indent += '\t';
        _blocks.push(block);
    }

    void beginPost()
    {
        _stream << ">" << std::endl;
    }

    void endInline()
    {
        _stream << "/>" << std::endl;
        _indent.erase(_indent.end() - 1);
        _blocks.pop();
    }

    void end()
    {
        _indent.erase(_indent.end() - 1);
        _stream << _indent << "</" << _blocks.top() << ">" << std::endl;
        _blocks.pop();
    }

    template<typename T>
    void assign(const std::string &name, const T &value)
    {
        _stream << name << "=\"" << value << "\" ";
    }

    template<typename T>
    void convert(const std::string &blockname, const std::string &name, const T &v)
    {
        begin(blockname);
        assign("name", name);
        assign("value", v);
        endInline();
    }

    template<typename Type, unsigned Size>
    void convert(const std::string &blockname, const std::string &name, const Vec<Type, Size> &v)
    {
        begin(blockname);
        assign("name", name);
        _stream << "value=\"";
        for (unsigned i = 0; i < Size; ++i)
            _stream << v[i] << (i == Size - 1 ? "" : ", ");
        _stream << "\"";
        endInline();
    }

    void convert(const std::string &name, const Mat4f &v)
    {
        begin("transform");
        assign("name", name);
        beginPost();
        begin("matrix");
        _stream << "value=\"";
        for (int i = 0; i < 15; ++i)
            _stream << v[i] << " ";
        _stream << v[15];
        _stream << "\"";
        endInline();
        end();
    }

    void convert(const std::string &name, bool v)
    {
        convert("boolean", name, v ? "true" : "false");
    }

    void convert(const std::string &name, int v)
    {
        convert("integer", name, v);
    }

    void convert(const std::string &name, float v)
    {
        convert("float", name, v);
    }

    void convert(const std::string &name, const char *v)
    {
        convert("string", name, v);
    }

    void convert(const std::string &name, const std::string &v)
    {
        convert("string", name, v);
    }

    template<typename Type, unsigned Size>
    void convertPoint(const std::string &name, const Vec<Type, Size> &v)
    {
        begin("point");
        assign("name", name);
        for (unsigned i = 0; i < Size; ++i)
            assign(std::string(1, 'x' + i), v[i]);
        endInline();
    }

    template<typename Type, unsigned Size>
    void convertVector(const std::string &name, const Vec<Type, Size> &v)
    {
        begin("vector");
        assign("name", name);
        for (int i = 0; i < Size; ++i)
            assign(std::string(1, 'x' + i), v[i]);
        endInline();
    }

    void convertSpectrum(const std::string &name, float v)
    {
        convert(name, v);
    }

    void convertSpectrum(const std::string &name, const Vec3f &v)
    {
        convert("rgb", name, v);
    }

    void convert(const std::string &name, ConstantTexture *c)
    {
        convertSpectrum(name, c->average());
    }

    void convert(const std::string &name, CheckerTexture *c)
    {
        begin("texture");
        if (!name.empty())
            assign("name", name);
        assign("type", "checkerboard");
        beginPost();
        convertSpectrum("color1", c->offColor());
        convertSpectrum("color0", c->onColor());
        convert("uoffset", 0.0f);
        convert("voffset", 0.0f);
        convert("uscale", float(c->resU())*0.5f);
        convert("vscale", float(c->resV())*0.5f);
        end();
    }

    void convert(const std::string &name, BitmapTexture *c)
    {
        begin("texture");
        if (!name.empty())
            assign("name", name);
        assign("type", "bitmap");
        beginPost();
        convert("filename", c->path()->asString());
        convert("filterType", "trilinear");
        end();
    }

    void convert(const std::string &name, Texture *a)
    {
        if (ConstantTexture *a2 = dynamic_cast<ConstantTexture *>(a))
            convert(name, a2);
        else if (BitmapTexture *a2 = dynamic_cast<BitmapTexture *>(a))
            convert(name, a2);
        else if (CheckerTexture *a2 = dynamic_cast<CheckerTexture *>(a))
            convert(name, a2);
        else
            DBG("Unknown texture type!");
    }

    template<typename T>
    void convertOrRef(T *x);

    void convert(HomogeneousMedium *med)
    {
        begin("medium");
        assign("type", "homogeneous");
        if (!med->unnamed())
            assign("name", med->name());
        beginPost();
        convertSpectrum("sigmaS", med->sigmaS());
        convertSpectrum("sigmaA", med->sigmaA());
    }

    void convert(Medium *med)
    {
        if (HomogeneousMedium *med2 = dynamic_cast<HomogeneousMedium *>(med))
            convert(med2);
        else
            DBG("Unknown medium type!");

        switch(med->phaseFunctionType()) {
        case PhaseFunction::Isotropic:
            break;
        case PhaseFunction::HenyeyGreenstein:
            begin("phase");
            assign("type", "hg");
            beginPost();
            convert("g", med->phaseG());
            end();
            break;
        case PhaseFunction::Rayleigh:
            begin("phase");
            assign("type", "rayleigh");
            endInline();
        }

        end();
    }

    void convert(LambertBsdf *bsdf)
    {
        begin("bsdf");
        assign("type", "diffuse");
        if (!bsdf->unnamed())
            assign("id", bsdf->name());
        beginPost();
        convert("reflectance", bsdf->albedo().get());
        end();
    }

    void convert(OrenNayarBsdf *bsdf)
    {
        begin("bsdf");
        assign("type", "roughdiffuse");
        if (!bsdf->unnamed())
            assign("id", bsdf->name());
        beginPost();
        convert("reflectance", bsdf->albedo().get());
        convert("alpha", bsdf->roughness().get());
        end();
    }

    void convert(PhongBsdf *bsdf)
    {
        begin("bsdf");
        assign("type", "phong");
        if (!bsdf->unnamed())
            assign("id", bsdf->name());
        beginPost();
        convert("specularReflectance", bsdf->albedo().get());
        convert("exponent", bsdf->exponent());
        convertSpectrum("diffuseReflectance", Vec3f(0.0f));
        end();
    }

    void convert(MixedBsdf *bsdf)
    {
        begin("bsdf");
        assign("type", "blendbsdf");
        if (!bsdf->unnamed())
            assign("id", bsdf->name());
        beginPost();
        convert("weight", bsdf->ratio().get());
        convertOrRef(bsdf->bsdf1().get());
        convertOrRef(bsdf->bsdf0().get());
        end();
    }

    void convert(DielectricBsdf *bsdf)
    {
        begin("bsdf");
        assign("type", "dielectric");
        if (!bsdf->unnamed())
            assign("id", bsdf->name());
        beginPost();
        convert("intIOR", bsdf->ior());
        convert("extIOR", 1.0f);
        end();
    }

    void convert(ThinSheetBsdf *bsdf)
    {
        begin("bsdf");
        assign("type", "thindielectric");
        if (!bsdf->unnamed())
            assign("id", bsdf->name());
        beginPost();
        convert("intIOR", bsdf->ior());
        convert("extIOR", 1.0f);
        end();
    }

    void convert(MirrorBsdf *bsdf)
    {
        begin("bsdf");
        assign("type", "conductor");
        if (!bsdf->unnamed())
            assign("id", bsdf->name());
        beginPost();
        convert("material", "none");
        end();
    }

    void convert(PlasticBsdf *bsdf)
    {
        begin("bsdf");
        assign("type", "plastic");
        if (!bsdf->unnamed())
            assign("id", bsdf->name());
        beginPost();
        convert("intIOR", bsdf->ior());
        convert("extIOR", 1.0f);
        convert("nonlinear", true);
        convert("diffuseReflectance", bsdf->albedo().get());
        end();
    }

    void convert(ConductorBsdf *bsdf)
    {
        begin("bsdf");
        assign("type", "conductor");
        if (!bsdf->unnamed())
            assign("id", bsdf->name());
        beginPost();
        convert("extEta", 1.0f);
        convert("specularReflectance", bsdf->albedo().get());
        convertSpectrum("eta", bsdf->eta());
        convertSpectrum("k", bsdf->k());
        end();
    }

    void convert(RoughConductorBsdf *bsdf)
    {
        begin("bsdf");
        assign("type", "roughconductor");
        if (!bsdf->unnamed())
            assign("id", bsdf->name());
        beginPost();
        convert("alpha", bsdf->roughness().get());
        convert("distribution", bsdf->distributionName());
        convert("extEta", 1.0f);
        convert("specularReflectance", bsdf->albedo().get());
        convertSpectrum("eta", bsdf->eta());
        convertSpectrum("k", bsdf->k());
        end();
    }

    void convert(RoughDielectricBsdf *bsdf)
    {
        begin("bsdf");
        assign("type", "roughdielectric");
        if (!bsdf->unnamed())
            assign("id", bsdf->name());
        beginPost();
        convert("alpha", bsdf->roughness().get());
        convert("distribution", bsdf->distributionName());
        convert("intIOR", bsdf->ior());
        convert("extIOR", 1.0f);
        end();
    }

    void convert(SmoothCoatBsdf *bsdf)
    {
        begin("bsdf");
        assign("type", "coating");
        if (!bsdf->unnamed())
            assign("id", bsdf->name());
        beginPost();
        convert("intIOR", bsdf->ior());
        convert("extIOR", 1.0f);
        convert("thickness", bsdf->thickness());
        convertSpectrum("sigmaA", bsdf->sigmaA());
        convertOrRef(bsdf->substrate().get());
        end();
    }

    void convert(NullBsdf *bsdf)
    {
        begin("bsdf");
        assign("type", "diffuse");
        if (!bsdf->unnamed())
            assign("id", bsdf->name());
        beginPost();
        convertSpectrum("reflectance", Vec3f(0.0f));
        end();
    }

    void convert(TransparencyBsdf *bsdf)
    {
        begin("bsdf");
        assign("type", "mask");
        if (!bsdf->unnamed())
            assign("id", bsdf->name());
        beginPost();
        convert("opacity", bsdf->opacity().get());
        convertOrRef(bsdf->base().get());
        end();
    }

    void convert(Bsdf *bsdf)
    {
        bool hasBump = bsdf->bump() && !bsdf->bump()->isConstant();
        if (hasBump) {
            begin("bsdf");
            assign("type", "bumpmap");
            beginPost();
            convert("map", bsdf->bump().get());
        }

        if (LambertBsdf *bsdf2 = dynamic_cast<LambertBsdf *>(bsdf))
            convert(bsdf2);
        else if (PhongBsdf *bsdf2 = dynamic_cast<PhongBsdf *>(bsdf))
            convert(bsdf2);
        else if (MixedBsdf *bsdf2 = dynamic_cast<MixedBsdf *>(bsdf))
            convert(bsdf2);
        else if (DielectricBsdf *bsdf2 = dynamic_cast<DielectricBsdf *>(bsdf))
            convert(bsdf2);
        else if (MirrorBsdf *bsdf2 = dynamic_cast<MirrorBsdf *>(bsdf))
            convert(bsdf2);
        else if (ConductorBsdf *bsdf2 = dynamic_cast<ConductorBsdf *>(bsdf))
            convert(bsdf2);
        else if (RoughConductorBsdf *bsdf2 = dynamic_cast<RoughConductorBsdf *>(bsdf))
            convert(bsdf2);
        else if (RoughDielectricBsdf *bsdf2 = dynamic_cast<RoughDielectricBsdf *>(bsdf))
            convert(bsdf2);
        else if (SmoothCoatBsdf *bsdf2 = dynamic_cast<SmoothCoatBsdf *>(bsdf))
            convert(bsdf2);
        else if (NullBsdf *bsdf2 = dynamic_cast<NullBsdf *>(bsdf))
            convert(bsdf2);
        else if (ThinSheetBsdf *bsdf2 = dynamic_cast<ThinSheetBsdf *>(bsdf))
            convert(bsdf2);
        else if (OrenNayarBsdf *bsdf2 = dynamic_cast<OrenNayarBsdf *>(bsdf))
            convert(bsdf2);
        else if (PlasticBsdf *bsdf2 = dynamic_cast<PlasticBsdf *>(bsdf))
            convert(bsdf2);
        else if (TransparencyBsdf *bsdf2 = dynamic_cast<TransparencyBsdf *>(bsdf))
            convert(bsdf2);
        else if (dynamic_cast<ForwardBsdf *>(bsdf)) {
        } else
            DBG("Unknown bsdf type with name '%s'!", bsdf->name());

        if (hasBump)
            end();
    }

    void convert(PinholeCamera *cam)
    {
        begin("sensor");
        assign("type", "perspective");
        beginPost();
        convert("fov", cam->fovDeg());
    }

    void convert(ThinlensCamera *cam)
    {
        begin("sensor");
        assign("type", "thinlens");
        beginPost();
        convert("fov", cam->fovDeg());
        convert("focusDistance", cam->focusDist());
        convert("apertureRadius", cam->apertureSize());
    }

    void convert(Camera *cam)
    {
        if (PinholeCamera *cam2 = dynamic_cast<PinholeCamera *>(cam))
            convert(cam2);
        else if (ThinlensCamera *cam2 = dynamic_cast<ThinlensCamera *>(cam))
            convert(cam2);
        else
            DBG("Unknown camera type!");

        convert("toWorld", cam->transform()*Mat4f::scale(Vec3f(-1.0f, 1.0f, 1.0f)));

        begin("sampler");
        if (_scene.rendererSettings().useSobol())
            assign("type", "sobol");
        else
            assign("type", "independent");
        beginPost();
        convert("sampleCount", int(_scene.rendererSettings().spp()));
        end();

        begin("film");
        assign("type", "ldrfilm");
        beginPost();
        convert("width", int(cam->resolution().x()));
        convert("height", int(cam->resolution().y()));
        convert("fileFormat", "png");
        convert("pixelFormat", "rgb");
        convert("gamma", 2.2f);
        convert("banner", false);

        begin("rfilter");
        assign("type", "box");
        endInline();

        end();

        end();
    }

    void convert(TriangleMesh *prim)
    {
        begin("shape");
        assign("type", "obj");
        beginPost();
        Path objFile = prim->path()->setExtension(".obj");
        Path fullObjFile;
        if (_folder.empty())
            fullObjFile = objFile;
        else {
            fullObjFile = _folder/objFile;
            if (!FileUtils::createDirectory(fullObjFile.parent()))
                DBG("Unable to create target folder for obj at: '%s'", fullObjFile);
        }
        prim->saveAsObj(fullObjFile);
        convert("filename", objFile.asString());
        convert("toWorld", prim->transform());
    }

    void convert(Sphere *prim)
    {
        begin("shape");
        assign("type", "sphere");
        beginPost();
        convert("radius", prim->radius());
        convertPoint("center", prim->pos());
    }

    void convert(Quad *prim)
    {
        begin("shape");
        assign("type", "rectangle");
        beginPost();
        convert("toWorld", prim->transform()*
                Mat4f::rotXYZ(Vec3f(90.0f, 0.0f, 0.0f))*
                Mat4f::scale(Vec3f(0.5f)));
    }

    void convert(InfiniteSphere *prim)
    {
        if (prim->emission()->isConstant()) {
            begin("emitter");
            assign("type", "constant");
            beginPost();
            convertSpectrum("radiance", prim->emission()->average());
            end();
        } else if (BitmapTexture *tex = dynamic_cast<BitmapTexture *>(prim->emission().get())) {
            begin("emitter");
            assign("type", "envmap");
            beginPost();
            convert("toWorld", prim->transform()*Mat4f::rotXYZ(Vec3f(0.0f, 90.0f, 0.0f)));
            convert("filename", tex->path()->setExtension(".hdr").asString());
            end();
        } else {
            DBG("Infinite sphere has to be a constant or bitmap textured light source!");
        }
    }

    void convert(Primitive *prim)
    {
        if (prim->numBsdfs() > 1)
            return; // Mitsuba does not support multiple BSDFs per primitive

        prim->prepareForRender();

        if (TriangleMesh *prim2 = dynamic_cast<TriangleMesh *>(prim))
            convert(prim2);
        else if (Sphere *prim2 = dynamic_cast<Sphere *>(prim))
            convert(prim2);
        else if (Quad *prim2 = dynamic_cast<Quad *>(prim))
            convert(prim2);
        else if (InfiniteSphere *prim2 = dynamic_cast<InfiniteSphere *>(prim)) {
            convert(prim2);
            return;
        } else
            DBG("Unknown primitive type!");

        if (!dynamic_cast<ForwardBsdf *>(prim->bsdf(0).get()))
            convertOrRef(prim->bsdf(0).get());
        if (prim->intMedium()) {
            prim->intMedium()->setName("interior");
            convert(prim->intMedium().get());
        }
        if (prim->extMedium()) {
            prim->extMedium()->setName("exterior");
            convert(prim->extMedium().get());
        }
        if (prim->isEmissive()) {
            begin("emitter");
            assign("type", "area");
            beginPost();
            convert("radiance", prim->emission().get());//*prim->area());
            end();
        }
        end();
    }

    void convert(Scene *scene)
    {
        begin("scene");
        assign("version", "0.5.0");
        beginPost();

        begin("integrator");
        if (scene->media().empty())
            assign("type", "path");
        else
            assign("type", "volpath");
        beginPost();
        convert("strictNormals", true);
        convert("maxDepth", 64);
        end();

        convert(scene->camera().get());

        for (const std::shared_ptr<Bsdf> &bsdf : scene->bsdfs())
            if (!bsdf->unnamed())
                convert(bsdf.get());
        for (const std::shared_ptr<Primitive> &prim : scene->primitives())
            convert(prim.get());

        end();
    }

public:
    SceneXmlWriter(const Path &folder, Scene &scene, std::ostream &stream)
    : _folder(folder),
      _stream(stream),
      _scene(scene)
    {
        _stream << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n\n";

        convert (&scene);
    }
};

template<typename T>
void SceneXmlWriter::convertOrRef(T *x)
{
    if (x->unnamed())
        convert(x);
    else {
        begin("ref");
        assign("id", x->name());
        endInline();
    }
}

}



#endif /* JSONXMLCONVERTER_HPP_ */
