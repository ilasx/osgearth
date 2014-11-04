/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
* Copyright 2008-2012 Pelican Mapping
* http://osgearth.org
*
* osgEarth is free software; you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU Lesser General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>
*/
#include "SplatTerrainEffect"

#include <osgEarth/Registry>
#include <osgEarth/Capabilities>
#include <osgEarth/VirtualProgram>
#include <osgEarth/TerrainEngineNode>
#include <osgEarth/ImageUtils>
#include <osgEarth/URI>

#include "NoiseShaders"
#include "SplatShaders"

#define LC "[Splat] "

#define COVERAGE_SAMPLER "oe_splat_coverage_tex"
#define SPLAT_SAMPLER    "oe_splat_tex"
#define SPLAT_FUNC       "oe_splat_getTexel"

// Tile LOD offset of the "Level 0" splatting scale. This is necessary
// to get rid of precision issues when scaling the splats up high.
//#define L0_OFFSET "10.0"

using namespace osgEarth;
using namespace osgEarth::Splat;

SplatTerrainEffect::SplatTerrainEffect(SplatCatalog*         catalog,
                                       SplatCoverageLegend*  legend,
                                       const osgDB::Options* dbOptions) :
_legend     ( legend ),
_ok         ( false ),
_renderOrder( -1.0f )
{
    if ( catalog )
    {
        _ok = catalog->createSplatTextureDef(dbOptions, _splatDef);
        if ( !_ok )
        {
            OE_WARN << LC << "Failed to create texture array from splat catalog\n";
        }
    }

    _scaleOffsetUniform = new osg::Uniform("oe_splat_scaleOffset", 0.0f);
    _intensityUniform   = new osg::Uniform("oe_splat_intensity",   1.0f);
    _warpUniform        = new osg::Uniform("oe_splat_warp",      0.004f);
    _blurUniform        = new osg::Uniform("oe_splat_blur",        1.0f);
    _snowUniform        = new osg::Uniform("oe_splat_snow",    10000.0f);

    _edit = (::getenv("OSGEARTH_SPLAT_EDIT") != 0L);
}

void
SplatTerrainEffect::onInstall(TerrainEngineNode* engine)
{
    if ( engine && _ok )
    {
        if ( !_coverageLayer.valid() )
        {
            OE_WARN << LC << "No coverage layer set\n";
            return;
        }

        osg::StateSet* stateset = engine->getOrCreateStateSet();

        // install the splat texture array:
        if ( engine->getTextureCompositor()->reserveTextureImageUnit(_splatTexUnit) )
        {
            // splat sampler
            _splatTexUniform = stateset->getOrCreateUniform( SPLAT_SAMPLER, osg::Uniform::SAMPLER_2D_ARRAY );
            _splatTexUniform->set( _splatTexUnit );
            stateset->setTextureAttribute( _splatTexUnit, _splatDef._texture.get(), osg::StateAttribute::ON );

            // coverage sampler
            _coverageTexUniform = stateset->getOrCreateUniform( COVERAGE_SAMPLER, osg::Uniform::SAMPLER_2D );
            _coverageTexUniform->set( _coverageLayer->shareImageUnit().get() );

            // control uniforms
            stateset->addUniform( _scaleOffsetUniform.get() );
            stateset->addUniform( _intensityUniform.get() );
            stateset->addUniform( _warpUniform.get() );
            stateset->addUniform( _blurUniform.get() );
            stateset->addUniform( _snowUniform.get() );

            stateset->getOrCreateUniform("oe_splat_freq", osg::Uniform::FLOAT)->set(32.0f);
            stateset->getOrCreateUniform("oe_splat_pers", osg::Uniform::FLOAT)->set(0.8f);
            stateset->getOrCreateUniform("oe_splat_lac",  osg::Uniform::FLOAT)->set(2.2f);
            stateset->getOrCreateUniform("oe_splat_octaves", osg::Uniform::FLOAT)->set(7.0f);
            stateset->getOrCreateUniform("oe_splat_saturate", osg::Uniform::FLOAT)->set(0.98f);
            stateset->getOrCreateUniform("oe_splat_thresh", osg::Uniform::FLOAT)->set(0.57f);
            stateset->getOrCreateUniform("oe_splat_slopeFactor", osg::Uniform::FLOAT)->set(0.47f);

            stateset->getOrCreateUniform("oe_splat_blending_range", osg::Uniform::FLOAT)->set(250000.0f);
            stateset->getOrCreateUniform("oe_splat_detail_range", osg::Uniform::FLOAT)->set(100000.0f);

            // Configure the vertex shader:
            std::string vertexShader = splatVertexShader;
            osgEarth::replaceIn( vertexShader, "$COVERAGE_TEXMAT_UNIFORM", _coverageLayer->shareTexMatUniformName().get() );
            
            // Configure the fragment shader:
            std::string fragmentShader = splatFragmentShader;
            std::string samplingCode = generateSamplingCode();
            osgEarth::replaceIn( fragmentShader, "$COVERAGE_SELECT_INDICES", samplingCode );

            if ( _edit )
                osgEarth::replaceIn( fragmentShader, "$SPLAT_EDIT", "#define SPLAT_EDIT 1\n" );
            else
                osgEarth::replaceIn( fragmentShader, "$SPLAT_EDIT", "" );

            // shader components
            VirtualProgram* vp = VirtualProgram::getOrCreate(stateset);
            vp->setFunction( "oe_splat_vertex",   vertexShader,   ShaderComp::LOCATION_VERTEX_VIEW );
            vp->setFunction( "oe_splat_fragment", fragmentShader, ShaderComp::LOCATION_FRAGMENT_COLORING, _renderOrder );

            // support shaders
            osg::Shader* noiseShader = new osg::Shader(osg::Shader::FRAGMENT, noiseShaders);
            vp->setShader( NOISE_FUNC, noiseShader );
        }
    }
}


void
SplatTerrainEffect::onUninstall(TerrainEngineNode* engine)
{
    osg::StateSet* stateset = engine->getStateSet();
    if ( stateset )
    {
        if ( _splatTexUniform.valid() )
        {
            stateset->removeUniform( _scaleOffsetUniform.get() );
            stateset->removeUniform( _warpUniform.get() );
            stateset->removeUniform( _blurUniform.get() );
            stateset->removeUniform( _snowUniform.get() );
            stateset->removeUniform( _intensityUniform.get() );
            stateset->removeUniform( _splatTexUniform.get() );
            stateset->removeUniform( _coverageTexUniform.get() );
            stateset->removeTextureAttribute( _splatTexUnit, osg::StateAttribute::TEXTURE );

            stateset->removeUniform( "oe_splat_freq" );
            stateset->removeUniform( "oe_splat_pers" );
            stateset->removeUniform( "oe_splat_lac" );
            stateset->removeUniform( "oe_splat_octaves" );
            stateset->removeUniform( "oe_splat_saturate" );
            stateset->removeUniform( "oe_splat_thresh" );
            stateset->removeUniform( "oe_splat_slopeFactor" );

            stateset->removeUniform( "oe_splat_blending_range" );
            stateset->removeUniform( "oe_splat_detail_range" );
        }

        VirtualProgram* vp = VirtualProgram::get(stateset);
        if ( vp )
        {
            vp->removeShader( "oe_splat_vertex" );
            vp->removeShader( "oe_splat_fragment" );
            vp->removeShader( SPLAT_FUNC );
            vp->removeShader( NOISE_FUNC );
        }
    }
    
    if ( _splatTexUnit >= 0 )
    {
        engine->getTextureCompositor()->releaseTextureImageUnit( _splatTexUnit );
        _splatTexUnit = -1;
    }
}

#define IND "    "

std::string
SplatTerrainEffect::generateSamplingCode()
{
    std::stringstream buf;

    unsigned count = 0;
    const SplatCoverageLegend::Predicates& preds = _legend->getPredicates();
    for(SplatCoverageLegend::Predicates::const_iterator p = preds.begin(); p != preds.end(); ++p, ++count)
    {
        const CoverageValuePredicate* pred = p->get();

        if ( pred->_exactValue.isSet() )
        {
            if ( count > 0 ) buf << IND "else ";
            else             buf << IND ;

            buf << "if (abs(value-float(" << pred->_exactValue.get() << "))<0.001) { \n";
            
            // Look up by class name:
            const std::string& className = pred->_mappedClassName.get();
            const SplatLUT::const_iterator i = _splatDef._splatLUT.find(className);
            if ( i != _splatDef._splatLUT.end() )
            {
                // found it; loop over the range selectors:
                int selectorCount = 0;
                const SplatSelectorVector& selectors = i->second;

                OE_DEBUG << LC << "Class " << className << " has " << selectors.size() << " selectors.\n";

                for(SplatSelectorVector::const_iterator selector = selectors.begin();
                    selector != selectors.end();
                    ++selector)
                {
                    const std::string&    expression = selector->first;
                    const SplatRangeData& rangeData  = selector->second;

                    if ( selectorCount > 0 ) buf << IND IND "else ";
                    else                     buf << IND IND ;

                    if ( !expression.empty() )
                    {
                        buf << "if (" << expression << ") \n";
                    }
                    buf << IND IND "{ \n";

                    buf << IND IND IND "primary = float(" << rangeData._textureIndex << "); \n";

                    if ( rangeData._detail->_textureIndex >= 0 )
                    {
                        buf << IND IND IND "detail  = float(" << rangeData._detail->_textureIndex << "); \n";
                        if ( rangeData._detail->_saturation.isSet() )
                            buf << IND IND IND "saturation = float(" << rangeData._detail->_saturation.get() << "); \n";
                        if ( rangeData._detail->_threshold.isSet() )
                            buf << IND IND IND "threshold = float(" << rangeData._detail->_threshold.get() << "); \n";
                        if ( rangeData._detail->_slope.isSet() )
                            buf << IND IND IND "slope = float(" << rangeData._detail->_slope.get() << "); \n";
                    }
                    buf << IND IND "}\n";

                    ++selectorCount;

                    // once we find an empty expression, we are finished because any
                    // subsequent selectors are unreachable.
                    if ( expression.empty() )
                        break;
                }
            }

            buf << IND "}\n";
        }
    }

    return buf.str();
}
