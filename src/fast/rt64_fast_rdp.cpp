#include "rt64_fast_state.h"

namespace RT64 {
namespace {
    uint32_t reverseWord(uint32_t value) {
        return (value<<24)|((value&0xff00U)<<8)|((value>>8)&0xff00U)|(value>>24);
    }
    std::array<float, 4> unpack(uint32_t color) {
        return {((color >> 24) & 255) / 255.0f, ((color >> 16) & 255) / 255.0f,
            ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f};
    }
    std::array<uint8_t, 4> rgba16(uint16_t value) {
        auto expand = [](uint32_t v) { return uint8_t((v << 3) | (v >> 2)); };
        return {expand(value >> 11), expand((value >> 6) & 31), expand((value >> 1) & 31), uint8_t((value & 1) ? 255 : 0)};
    }
    std::vector<FastVertex> rectangle(const FastDraw &draw, float x0, float y0, float x1, float y1) {
        std::vector<FastVertex> vertices(6);
        const float xy[6][2] = {{x0,y0},{x1,y0},{x1,y1},{x0,y0},{x1,y1},{x0,y1}};
        for (unsigned i = 0; i < 6; ++i) {
            vertices[i].position[0] = xy[i][0] * 2 / draw.width - 1;
            vertices[i].position[1] = 1 - xy[i][1] * 2 / draw.height;
        }
        return vertices;
    }
}
    void FastRDP::setColorImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address) {
        if (fmt != G_IM_FMT_RGBA || (siz != G_IM_SIZ_16b && siz != G_IM_SIZ_32b) || !width || width > 1024)
            throw std::runtime_error("RT64 Fast unsupported color image format or width");
        parameters.colorAddress = address & 0xffffff;
        parameters.width = width;
        // SetColorImage encodes width but not height. Seed a console-aspect
        // surface and let scissor extents grow it before drawing. In particular,
        // DK64's opening 640-wide mode needs 480 lines, not a fixed 240 lines.
        parameters.height = std::max(1U, uint32_t(width) * 3 / 4);
        parameters.colorBytes = siz == G_IM_SIZ_32b ? 4 : 2;
        colorSize = siz;
    }
    void FastRDP::setDepthImage(uint32_t address) { parameters.depthAddress = address & 0xffffff; }
    void FastRDP::setTextureImage(uint8_t fmt, uint8_t siz, uint16_t width, uint32_t address) {
        textureAddress = address & 0xffffff;
        textureWidth = width; textureSize = siz; textureFormat = fmt;
    }
    void FastRDP::setCombine(uint64_t value) { parameters.combine = {uint32_t(value), uint32_t(value >> 32)}; }
    void FastRDP::setTile(uint8_t tile, uint8_t fmt, uint8_t siz, uint16_t line, uint16_t address,
        uint8_t palette, uint8_t cmt, uint8_t cms, uint8_t maskt, uint8_t masks, uint8_t shiftt, uint8_t shifts) {
        auto &t = tiles.at(tile);
        t.fmt=fmt; t.siz=siz; t.line=line; t.tmem=address; t.palette=palette;
        t.cmt=cmt; t.cms=cms; t.maskt=maskt; t.masks=masks; t.shiftt=shiftt; t.shifts=shifts;
        decodedTextures[tile].reset();
    }
    void FastRDP::setTileSize(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
        auto &t = tiles.at(tile);
        t.uls=uls; t.ult=ult; t.lrs=lrs; t.lrt=lrt;
        decodedTextures[tile].reset();
    }

    // Same bank split and odd-row XOR as RT64's RDP loadToTMEMCommon. Reads use
    // the runtime's word-swapped RDRAM layout; TMEM itself is byte addressed.
    void FastRDP::loadTMEM(uint8_t tile, uint32_t start, uint32_t stride, uint32_t words,
        uint32_t rows, bool block, bool palette, uint16_t dxt) {
        const auto &t = tiles.at(tile);
        const bool rgba32 = t.fmt == G_IM_FMT_RGBA && t.siz == G_IM_SIZ_32b;
        const uint32_t mask = rgba32 ? 2047 : 4095, advance = rgba32 ? 4 : 8;
        const uint32_t tmemStride = uint32_t(t.line) << (palette ? 5 : 3);
        if(!rows || !words) throw std::invalid_argument("RT64 Fast empty TMEM transfer");
        const uint64_t rowOffset=uint64_t(rows-1)*stride,rowBytes=uint64_t(words)*(palette?2:8);
        // Check before adding or narrowing to size_t on the 32-bit target.
        if(rowOffset>state->rdramSize || rowBytes>state->rdramSize-rowOffset)
            throw std::out_of_range("RT64 Fast TMEM source span exceeds RDRAM");
        const uint64_t span=rowOffset+rowBytes;
        state->fromRDRAM(start,span);
        auto framebuffer=state->sink.snapshotFramebuffer(start,uint32_t(span));
        uint32_t load=0;
        std::shared_ptr<const std::vector<uint8_t>> loadedBytes;
        if(framebuffer) {
            do { ++nextFramebufferLoad; } while(!nextFramebufferLoad || framebufferLoads.count(nextFramebufferLoad));
            load=nextFramebufferLoad;
            for(const auto &entry:framebufferLoads)
                if(entry.second.image==framebuffer && entry.second.bytes) { loadedBytes=entry.second.bytes; break; }
            framebufferLoads.emplace(load,FramebufferLoad{framebuffer,loadedBytes,0});
        }
        // The whole source span was checked above. RDRAM's size is a multiple
        // of four, so the byte XOR stays within the supplied memory.
        // When there are no framebuffer references, their per-byte provenance
        // is already zero and tmemSource is unused.
        const bool plainMemory=!load && framebufferLoads.empty();
        const uint32_t endian=1;
        const bool wordTransfers=plainMemory && !palette && *reinterpret_cast<const uint8_t *>(&endian)==1;
        uint32_t dxtCounter = 0, swap = 0;
        for (uint32_t row = 0; row < rows; ++row) {
            uint32_t dst = ((uint32_t(t.tmem) << 3) + row * tmemStride) & mask;
            uint32_t src = start + row * stride;
            for (uint32_t word = 0; word < words; ++word) {
                if(wordTransfers && !(src&3)) {
                    uint32_t first,second;
                    std::memcpy(&first,state->RDRAM+src,4);
                    std::memcpy(&second,state->RDRAM+(src+4),4);
                    if(rgba32) {
                        // Split RG and BA into their TMEM banks, then store
                        // big-endian byte order with the same odd-row XOR.
                        const uint32_t rg=reverseWord((first&0xffff0000U)|(second>>16));
                        const uint32_t ba=reverseWord((first<<16)|(second&0xffffU));
                        const uint32_t at=dst^swap;
                        std::memcpy(tmem.data()+at,&rg,4);
                        std::memcpy(tmem.data()+(at|2048),&ba,4);
                    } else {
                        first=reverseWord(first); second=reverseWord(second);
                        const uint32_t at=dst^swap;
                        std::memcpy(tmem.data()+at,&first,4);
                        std::memcpy(tmem.data()+(((dst+4)&mask)^swap),&second,4);
                    }
                } else {
                    for (unsigned byte = 0; byte < 8; ++byte) {
                        const uint32_t source=src+(palette?(byte&1):byte);
                        const uint8_t v=loadedBytes?loadedBytes->at(source-framebuffer->address):state->RDRAM[source^3];
                        uint32_t destination;
                        if (rgba32) {
                            const uint32_t bank = (byte & 2) ? 2048 : 0;
                            const uint32_t offset = (byte / 4) * 2 + (byte & 1);
                            destination=(((dst+offset)&mask)^swap)|bank;
                        } else destination=((dst+byte)&mask)^swap;
                        tmem[destination]=v;
                        if(!plainMemory) setFramebufferByte(destination,load,source);
                    }
                }
                if (block) {
                    dxtCounter += dxt;
                    while (dxtCounter >= 2048) {
                        dst = (dst + tmemStride) & mask;
                        dxtCounter -= 2048; swap ^= 4;
                    }
                }
                dst = (dst + advance) & mask;
                src += palette ? 2 : 8;
            }
            if (!block) swap ^= 4;
        }
        ++tmemGeneration;
    }
    void FastRDP::setFramebufferByte(uint32_t address,uint32_t load,uint32_t source) {
        const uint32_t old=tmemFramebuffer[address];
        if(old!=load) {
            if(old && !--framebufferLoads.at(old).references) framebufferLoads.erase(old);
            if(load) ++framebufferLoads.at(load).references;
            tmemFramebuffer[address]=load;
        }
        tmemSource[address]=source;
    }
    void FastRDP::materializeFramebufferTMEM(uint32_t load) {
        const auto framebuffer=framebufferLoads.at(load).image;
        auto bytes=std::make_shared<std::vector<uint8_t>>();
        if(!state->sink.readFramebufferSnapshot(*framebuffer,*bytes)
            || bytes->size()!=size_t(framebuffer->width)*framebuffer->height*framebuffer->colorBytes)
            throw std::runtime_error("RT64 Fast cannot read framebuffer TMEM reinterpretation");
        for(auto &entry:framebufferLoads) if(entry.second.image==framebuffer) entry.second.bytes=bytes;
        for(unsigned i=0;i<tmem.size();++i)
            if(tmemFramebuffer[i] && framebufferLoads.at(tmemFramebuffer[i]).image==framebuffer)
                tmem[i]=bytes->at(tmemSource[i]-framebuffer->address);
    }
    uint8_t FastRDP::readTMEM(uint32_t address) {
        address&=4095;
        const uint32_t load=tmemFramebuffer[address];
        if(load && !framebufferLoads.at(load).bytes) materializeFramebufferTMEM(load);
        return tmem[address];
    }
    bool FastRDP::decodeFramebufferView(const FastTile &tile,FastTexture &texture) {
        if(tile.fmt!=G_IM_FMT_RGBA || (tile.siz!=G_IM_SIZ_16b && tile.siz!=G_IM_SIZ_32b)) return false;
        const uint32_t bpp=tile.siz==G_IM_SIZ_16b?2:4;
        auto location=[&](unsigned x,unsigned y,unsigned byte) {
            const uint32_t row=uint32_t(tile.tmem)*8+y*tile.line*8,swap=(y&1)*4;
            if(bpp==4) return (((row+x*2+(byte&1))^swap)&2047) | ((byte&2)?2048:0);
            return (((row+x*2)^swap)+byte)&4095;
        };
        const uint32_t first=location(0,0,0),load=tmemFramebuffer[first];
        if(!load) return false;
        const auto &fb=framebufferLoads.at(load).image;
        if(fb->colorBytes!=bpp || !fb->texture || !fb->texture->storage || tmemSource[first]<fb->address) return false;
        const uint32_t offset=tmemSource[first]-fb->address;
        if(offset%bpp) return false;
        const uint32_t x0=(offset/bpp)%fb->width,y0=(offset/bpp)/fb->width;
        if(x0+texture.width>fb->width || y0+texture.height>fb->height) return false;
        for(unsigned y=0;y<texture.height;++y) for(unsigned x=0;x<texture.width;++x) for(unsigned byte=0;byte<bpp;++byte) {
            const uint32_t slot=location(x,y,byte),id=tmemFramebuffer[slot];
            if(!id || framebufferLoads.at(id).image!=fb
                || tmemSource[slot]!=tmemSource[first]+(y*fb->width+x)*bpp+byte) return false;
        }
        texture.storage=fb->texture->storage;
        texture.storageX=fb->texture->storageX+x0; texture.storageY=fb->texture->storageY+y0;
        const uint64_t key[]={fb->texture->hash,x0,y0,texture.width,texture.height};
        texture.hash=XXH3_64bits(key,sizeof(key));
        return true;
    }
    void FastRDP::loadTile(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
        if (lrs < uls || lrt < ult) throw std::runtime_error("RT64 Fast reversed tile load bounds");
        setTileSize(tile, uls, ult, lrs, lrt);
        const uint32_t stride = textureWidth << textureSize >> 1;
        const uint32_t start = textureAddress + ((uls >> 2) << textureSize >> 1) + (ult >> 2) * stride;
        loadTMEM(tile, start, stride, (((lrs >> 2) - (uls >> 2)) >> (4 - tiles[tile].siz)) + 1,
            (lrt >> 2) - (ult >> 2) + 1, false, false);
    }
    void FastRDP::loadBlock(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t dxt) {
        if (lrs < uls) throw std::runtime_error("RT64 Fast reversed block load bounds");
        setTileSize(tile, uls, ult, lrs, dxt);
        const uint32_t stride = textureWidth << textureSize >> 1;
        const uint32_t start = textureAddress + (uint32_t(uls) << textureSize >> 1) + uint32_t(ult) * stride;
        loadTMEM(tile, start, stride, ((lrs - uls) >> (4 - tiles[tile].siz)) + 1, 1, true, false, dxt);
    }
    void FastRDP::loadTLUT(uint8_t tile, uint16_t uls, uint16_t ult, uint16_t lrs, uint16_t lrt) {
        if (lrs < uls || lrt < ult) throw std::runtime_error("RT64 Fast reversed palette load bounds");
        const uint32_t stride = textureWidth << textureSize >> 1;
        const uint32_t start = textureAddress + ((uls >> 2) << textureSize >> 1) + (ult >> 2) * stride;
        loadTMEM(tile, start, stride, (lrs >> 2) - (uls >> 2) + 1, (lrt >> 2) - (ult >> 2) + 1, false, true);
    }
    std::shared_ptr<const FastTexture> FastRDP::decodeTexture(uint8_t tile) {
        auto &cached = decodedTextures.at(tile);
        const uint64_t generation = tmemGeneration * 4 + (otherMode.textLUT() >> G_MDSFT_TEXTLUT);
        if (cached && decodedGenerations[tile] == generation) return cached;
        const auto &t = tiles.at(tile);
        const uint32_t tileWidth = ((uint32_t(t.lrs) - t.uls) & 4095) / 4 + 1;
        const uint32_t tileHeight = ((uint32_t(t.lrt) - t.ult) & 4095) / 4 + 1;
        const uint32_t width=t.masks && !(t.cms & G_TX_CLAMP) ? (1U << t.masks) : tileWidth;
        const uint32_t height=t.maskt && !(t.cmt & G_TX_CLAMP) ? (1U << t.maskt) : tileHeight;
        if (width > 1024 || height > 1024) throw std::runtime_error("RT64 Fast texture exceeds 1024 texels");
        // Raw TMEM alone cannot identify GPU-backed bytes. Keep framebuffer
        // views and mixed/materialized loads on the existing provenance path.
        const bool cacheable=framebufferLoads.empty();
        const std::array<uint32_t,8> layout={t.fmt,t.siz,t.line,t.tmem,t.palette,width,height,otherMode.textLUT()};
        uint64_t key=0;
        if(cacheable) {
            key=XXH3_64bits_withSeed(tmem.data(),tmem.size(),XXH3_64bits(layout.data(),sizeof(layout)));
            const auto range=cpuTextureCache.equal_range(key);
            for(auto it=range.first;it!=range.second;++it) {
                auto &entry=it->second;
                // The hash is only an index: exact comparisons prevent a
                // collision from reusing another image or decode layout.
                if(entry.layout==layout && entry.memory==tmem) {
                    entry.used=++cpuTextureCacheClock;
                    cached=entry.texture; decodedGenerations[tile]=generation;
                    return cached;
                }
            }
        }
        auto out = std::make_shared<FastTexture>();
        out->width=width; out->height=height;
        if(decodeFramebufferView(t,*out)) {
            cached=out; decodedGenerations[tile]=generation; return out;
        }
        out->rgba.resize(size_t(out->width) * out->height * 4);
        auto u16 = [&](uint32_t a) { return uint16_t((uint16_t(readTMEM(a)) << 8) | readTMEM(a+1)); };
        for (uint32_t y = 0; y < out->height; ++y) for (uint32_t x = 0; x < out->width; ++x) {
            const uint32_t row = uint32_t(t.tmem) * 8 + y * t.line * 8, swap = (y & 1) * 4;
            const uint32_t address = (row + (x << t.siz >> 1)) ^ swap;
            const uint8_t byte = readTMEM(address);
            const uint8_t nibble = (byte >> ((x & 1) ? 0 : 4)) & 15;
            std::array<uint8_t, 4> pixel{};
            if (t.fmt == G_IM_FMT_RGBA && t.siz == G_IM_SIZ_16b) pixel = rgba16(u16(address));
            else if (t.fmt == G_IM_FMT_RGBA && t.siz == G_IM_SIZ_32b) {
                const uint32_t a = ((row + x * 2) ^ swap) & 2047;
                pixel = {readTMEM(a),readTMEM((a+1)&2047),readTMEM(a|2048),readTMEM(((a+1)&2047)|2048)};
            } else if (t.fmt == G_IM_FMT_CI && (t.siz == G_IM_SIZ_4b || t.siz == G_IM_SIZ_8b)) {
                const uint32_t index = t.siz == G_IM_SIZ_4b ? t.palette * 16 + nibble : byte;
                const uint16_t entry = u16(2048 + index * 8);
                if (otherMode.textLUT() == G_TT_RGBA16) pixel = rgba16(entry);
                else if (otherMode.textLUT() == G_TT_IA16) pixel = {uint8_t(entry>>8),uint8_t(entry>>8),uint8_t(entry>>8),uint8_t(entry)};
                else throw std::runtime_error("RT64 Fast CI texture without supported TLUT mode");
            } else if (t.fmt == G_IM_FMT_I && (t.siz == G_IM_SIZ_4b || t.siz == G_IM_SIZ_8b)) {
                const uint8_t i = t.siz == G_IM_SIZ_4b ? nibble * 17 : byte;
                pixel = {i,i,i,i};
            } else if (t.fmt == G_IM_FMT_IA && t.siz == G_IM_SIZ_4b) {
                const uint8_t raw = nibble >> 1, i = (raw << 5) | (raw << 2) | (raw >> 1);
                pixel = {i,i,i,uint8_t((nibble & 1) ? 255 : 0)};
            } else if (t.fmt == G_IM_FMT_IA && t.siz == G_IM_SIZ_8b) {
                const uint8_t i = (byte >> 4) * 17;
                pixel = {i,i,i,uint8_t((byte & 15) * 17)};
            } else if (t.fmt == G_IM_FMT_IA && t.siz == G_IM_SIZ_16b) {
                pixel = {byte,byte,byte,readTMEM(address+1)};
            } else throw std::runtime_error("RT64 Fast unsupported texture format/size");
            std::copy(pixel.begin(), pixel.end(), out->rgba.begin() + (size_t(y) * out->width + x) * 4);
        }
        out->hash = XXH3_64bits(out->rgba.data(), out->rgba.size());
        const size_t retainedBytes=sizeof(CachedCPUTexture)+out->rgba.size();
        if(cacheable && retainedBytes<=cpuTextureCacheLimit) {
            while(cpuTextureCacheBytes+retainedBytes>cpuTextureCacheLimit) {
                auto oldest=std::min_element(cpuTextureCache.begin(),cpuTextureCache.end(),
                    [](const auto &a,const auto &b){return a.second.used<b.second.used;});
                cpuTextureCacheBytes-=sizeof(CachedCPUTexture)+oldest->second.texture->rgba.size();
                cpuTextureCache.erase(oldest);
            }
            cpuTextureCache.emplace(key,CachedCPUTexture{layout,tmem,out,++cpuTextureCacheClock});
            cpuTextureCacheBytes+=retainedBytes;
        }
        cached = out; decodedGenerations[tile] = generation;
        return out;
    }
    void FastRDP::setEnvColor(uint32_t color) { parameters.environment = unpack(color); }
    void FastRDP::setPrimColor(uint8_t lodFrac, uint8_t, uint32_t color) {
        parameters.primitive = unpack(color); parameters.lodFraction = lodFrac / 255.0f;
    }
    void FastRDP::setBlendColor(uint32_t color) { parameters.blendColor = unpack(color); }
    void FastRDP::setFogColor(uint32_t color) { parameters.fogColor = unpack(color); }
    void FastRDP::setPrimDepth(uint16_t z, uint16_t) { primitiveDepth = std::min(z / 32767.0f, 1.0f) * 2 - 1; }
    void FastRDP::setScissor(uint8_t mode, int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
        if (mode != 0) throw std::runtime_error("RT64 Fast interlaced scissor not implemented");
        parameters.scissor = {ulx,uly,lrx,lry};
        parameters.height = std::max(parameters.height,uint32_t(std::max(1,(lry+3)/4)));
    }
    void FastRDP::setConvert(int32_t, int32_t, int32_t, int32_t, int32_t k4, int32_t k5) {
        auto signed9 = [](int32_t value) { return value & 256 ? value - 512 : value; };
        parameters.k4 = signed9(k4) / 255.0f; parameters.k5 = signed9(k5) / 255.0f;
    }
    void FastRDP::setKeyR(uint32_t center, uint32_t scale, uint32_t) {
        parameters.keyCenter[0] = center / 255.0f; parameters.keyScale[0] = scale / 255.0f;
    }
    void FastRDP::setKeyGB(uint32_t cg, uint32_t sg, uint32_t, uint32_t cb, uint32_t sb, uint32_t) {
        parameters.keyCenter[1] = cg / 255.0f; parameters.keyScale[1] = sg / 255.0f;
        parameters.keyCenter[2] = cb / 255.0f; parameters.keyScale[2] = sb / 255.0f;
    }
    FastDraw FastRDP::makeDraw(uint8_t tile, bool textured) {
        FastDraw draw = parameters;
        draw.memoryEpoch=state->memoryEpoch;
        draw.otherMode = otherMode;
        if (textured) for (unsigned i = 0; i < 2; ++i) {
            const uint8_t index = (tile + i) & 7;
            draw.tiles[i] = tiles[index];
            if (draw.combine.usesTexture(otherMode, i, false)) draw.textures[i] = decodeTexture(index);
        }
        return draw;
    }
    void FastRDP::fillRect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry) {
        auto draw = makeDraw(0, false);
        draw.rectangle = true;
        draw.fill = otherMode.cycleType() == G_CYC_FILL;
        draw.clearDepth = draw.fill && parameters.colorAddress == parameters.depthAddress;
        const float inclusive = draw.fill || otherMode.cycleType() == G_CYC_COPY ? 1.0f : 0.0f;
        draw.vertices = rectangle(draw, ulx / 4.0f, uly / 4.0f, lrx / 4.0f + inclusive, lry / 4.0f + inclusive);
        if (colorSize == G_IM_SIZ_16b) {
            auto color = rgba16(fillColor >> 16);
            for (unsigned i = 0; i < 4; ++i) draw.fillColor[i] = color[i] / 255.0f;
        } else draw.fillColor = unpack(fillColor);
        state->sink.draw(draw);
    }
    void FastRDP::drawTexRect(int32_t ulx, int32_t uly, int32_t lrx, int32_t lry, uint8_t tile,
        int16_t uls, int16_t ult, int16_t dsdx, int16_t dtdy, bool flip) {
        auto draw = makeDraw(tile, true);
        draw.rectangle = true;
        const bool copy = otherMode.cycleType() == G_CYC_COPY;
        const float x0 = ulx / 4.0f, y0 = uly / 4.0f, x1 = lrx / 4.0f + (copy ? 1 : 0), y1 = lry / 4.0f + (copy ? 1 : 0);
        draw.vertices = rectangle(draw, x0, y0, x1, y1);
        const float xy[6][2] = {{x0,y0},{x1,y0},{x1,y1},{x0,y0},{x1,y1},{x0,y1}};
        for (unsigned i = 0; i < 6; ++i) {
            const float dx = xy[i][0] - x0, dy = xy[i][1] - y0;
            draw.vertices[i].uv[0] = uls / 32.0f + (flip ? dy : dx) * dsdx / (copy ? 4096.0f : 1024.0f);
            draw.vertices[i].uv[1] = ult / 32.0f + (flip ? dx : dy) * dtdy / 1024.0f;
            draw.vertices[i].position[2] = primitiveDepth;
        }
        draw.depthTest = otherMode.zSource() == G_ZS_PRIM && otherMode.zCmp();
        draw.depthWrite = otherMode.zSource() == G_ZS_PRIM && otherMode.zUpd();
        state->sink.draw(draw);
    }
    void FastRDP::drawTris(uint32_t count, const float *pos, const float *tc, const float *col, uint8_t tile, uint8_t) {
        auto draw = makeDraw(tile, true);
        draw.depthTest = otherMode.zCmp(); draw.depthWrite = otherMode.zUpd();
        draw.vertices.resize(count * 3);
        for (unsigned i = 0; i < count * 3; ++i) {
            auto &v = draw.vertices[i];
            const float w = pos[i*4+3];
            v.position[0] = (2 * pos[i*4] / draw.width - 1) * w;
            v.position[1] = (1 - 2 * pos[i*4+1] / draw.height) * w;
            v.position[2] = (2 * pos[i*4+2] - 1) * w; v.position[3] = w;
            std::copy(tc + i*2, tc + i*2+2, v.uv);
            std::copy(col + i*4, col + i*4+4, v.color);
        }
        state->sink.draw(draw);
    }
}
