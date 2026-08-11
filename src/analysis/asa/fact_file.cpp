/*
 * VestaVM -- Maquina Virtual Distribuida
 *
 * Copyright (C) 2026 David Lopez.T (DesmonHak) (Castilla y Leon, ES)
 * Licencia: GPLv2 + excepcion de runtime (ver LICENSE).
 */

/**
 * @file analysis/asa/fact_file.cpp
 * @brief Implementacion del fichero de hechos (ver @c analysis/asa/fact_file.h).
 */

#include "analysis/asa/fact_file.h"

#include "analysis/asa/base_hechos.h"

#include "util/fnv.h"
#include "util/fs_utils.h"
#include "util/serialize.h"

#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace analysis {
namespace asa {

namespace {

constexpr uint32_t kMagic = 0x46415856u;    ///< 'VXAF' en little-endian.
constexpr uint32_t kMagicEnd = 0x4E494658u; ///< 'XFIN': cierra el fichero.

/// Marca de "esta cadena no esta" en la tabla de un registro.  No es la cadena
/// cero: la cadena vacia es una entrada legitima.
constexpr uint32_t kNoString = 0xFFFFFFFFu;

/// Huella del nombre de un dominio, solo para indexarlo en la cola: la
/// coincidencia se CONFIRMA leyendo el nombre, asi que una colision no engaña.
uint64_t name_hash(const char *s) {
    if (s == nullptr) return util::kFnvOffset;
    return util::fnv_bytes(util::kFnvOffset, s, std::strlen(s));
}

/**
 * @brief Tabla de cadenas de UN registro.
 *
 * Por registro y no global a proposito: un lector que no conoce un dominio lo
 * SALTA entero, y no podria hacerlo si sus cadenas vivieran en una tabla
 * compartida que hay que leer si o si.  Es lo que hace que añadir un productor
 * no invalide las caches escritas antes.
 */
class StringTable {
  public:
    uint32_t index_of(const char *s) {
        const std::string k = (s == nullptr) ? std::string() : std::string(s);
        auto it = idx_.find(k);
        if (it != idx_.end()) return it->second;
        const uint32_t i = static_cast<uint32_t>(orden_.size());
        orden_.push_back(k);
        idx_.emplace(k, i);
        return i;
    }
    void escribir(util::ByteWriter &w) const {
        w.u32(static_cast<uint32_t>(orden_.size()));
        for (const std::string &s : orden_) w.str(s);
    }
    bool vacia() const { return orden_.empty(); }

  private:
    std::vector<std::string>                  orden_;
    std::unordered_map<std::string, uint32_t> idx_;
};

/**
 * @brief Devuelve una cadena leida a su literal canonico si lo tiene.
 *
 * Es LO QUE HACE QUE UN HECHO DE DISCO SIGA SIENDO EL MISMO HECHO: ASA compara
 * productores y dominios por direccion, asi que una cadena recien leida no se
 * reconoceria a si misma.  Si nadie dio de alta ese nombre -- un productor que
 * este build no tiene -- se interna: el hecho se conserva y se puede leer, pero
 * nadie de aqui va a preguntar por el con un literal que no existe.
 */
const char *canonicalize(FactStore &almacen, const std::string &s) {
    if (const char *c = canonical_name(s)) return c;
    return almacen.internar(s);
}

} // namespace

const char *diag_code(ReadReason m) {
    switch (m) {
    case ReadReason::NoFile: return "VXA036";
    case ReadReason::Empty: return "VXA037";
    case ReadReason::NotAFactFile: return "VXA038";
    case ReadReason::OtherVersion: return "VXA039";
    case ReadReason::OtherModule: return "VXA042";
    case ReadReason::Truncated: return "VXA043";
    case ReadReason::ReadFailed: return "VXA044";
    default: return "";
    }
}

CacheLevel cache_level() {
    /* Una vez por proceso: es una decision de configuracion, no cambia a mitad
     * de una compilacion, y consultarla es barato solo si no se relee. */
    static const CacheLevel n = [] {
        const char *v = std::getenv("VESTA_ASA_CACHE");
        if (v == nullptr || v[0] == '\0') return CacheLevel::ByCost;
        switch (v[0]) {
        case '0': return CacheLevel::Off;
        case '1': return CacheLevel::Minimum;
        case '3': return CacheLevel::All;
        default: return CacheLevel::ByCost;
        }
    }();
    return n;
}

const char *level_name(CacheLevel n) {
    switch (n) {
    case CacheLevel::Off: return "nada";
    case CacheLevel::Minimum: return "minimo";
    case CacheLevel::All: return "todo";
    default: return "por-coste";
    }
}

bool should_store(CacheLevel nivel, const DomainCost &c) {
    switch (nivel) {
    case CacheLevel::Off: return false;
    /* Lo imprescindible: lo que si no se guarda se perdio.  Los niveles son
     * monotonos, asi que esto entra tambien en los dos de arriba. */
    case CacheLevel::Minimum: return !c.recomputable;
    case CacheLevel::All: return true;
    default: break;
    }
    /* Por coste.  El umbral no es una constante inventada sino la respuesta a
     * "cuesta mas rehacerlo que leerlo": por debajo de un milisegundo,
     * recalcular sale igual de barato que abrir el fichero. */
    constexpr long kUmbralMicros = 1000;
    return !c.recomputable || c.micros >= kUmbralMicros;
}

std::vector<uint8_t> serialize(const FactStore                 &almacen,
                                uint64_t                         huella,
                                CacheLevel                       nivel,
                                const std::vector<DomainCost> &costes) {
    if (nivel == CacheLevel::Off || almacen.size() == 0) return {};

    /* Agrupar por dominio, indexando por el PUNTERO del nombre: dentro de un
     * almacen los hechos de un dominio comparten literal (es como el propio
     * FactStore los indexa), asi que esto evita construir una cadena por hecho.
     * Se conserva el orden de aparicion para que dos volcados del mismo modulo
     * den el mismo fichero y se puedan comparar. */
    std::vector<const char *>                             dominios;
    std::unordered_map<const char *, std::vector<FactId>> por_dominio;
    std::unordered_map<const char *, bool>                admitido;
    std::unordered_map<const char *, uint64_t>            huella_de;
    for (FactId id = 0; id < static_cast<FactId>(almacen.size()); ++id) {
        const Fact       &f = almacen.at(id);
        const char *const dom = f.que.dominio;
        auto              ad = admitido.find(dom);
        if (ad == admitido.end()) {
            /* El criterio se resuelve UNA vez por dominio: depende del dominio,
             * y preguntarlo por hecho seria repetir la misma respuesta cientos
             * de miles de veces. */
            DomainCost c;
            c.domain = dom;
            for (const DomainCost &q : costes) {
                if (q.domain != nullptr && dom != nullptr &&
                    std::strcmp(q.domain, dom) == 0) {
                    c = q;
                    break;
                }
            }
            huella_de[dom] = c.fingerprint;
            ad = admitido.emplace(dom, should_store(nivel, c)).first;
        }
        if (!ad->second) continue;
        auto it = por_dominio.find(dom);
        if (it == por_dominio.end()) {
            dominios.push_back(dom);
            it = por_dominio.emplace(dom, std::vector<FactId>{}).first;
        }
        it->second.push_back(id);
    }
    if (dominios.empty()) return {};

    util::ByteWriter w;
    /* Un hecho ocupa del orden de 80 bytes con su tabla de cadenas amortizada;
     * reservar de una evita una veintena de realojos en un modulo grande. */
    w.reserve(almacen.size() * 80 + 64);

    w.u32(kMagic);
    w.u16(kContainerVersion);
    w.u16(0); // reservado: alinea la huella a 8.
    w.u64(huella);
    w.u32(static_cast<uint32_t>(almacen.size())); // dimensiona el remapeo.
    w.u32(static_cast<uint32_t>(dominios.size()));

    struct IndexEntry {
        uint64_t hash = 0;
        uint64_t offset = 0;
    };
    std::vector<IndexEntry> indice;
    indice.reserve(dominios.size());

    for (const char *dom : dominios) {
        const std::vector<FactId> &ids = por_dominio[dom];
        IndexEntry              e;
        e.hash = name_hash(dom);
        e.offset = w.size();

        w.str(dom == nullptr ? std::string() : std::string(dom));
        w.u16(kFactVersion);
        /* La huella de LO QUE MIRO este dominio, no la del modulo: es lo que
         * permite tirar un registro y conservar los demas. */
        w.u64(huella_de[dom]);
        w.u32(static_cast<uint32_t>(ids.size()));
        /* La longitud del cuerpo no se sabe hasta escribirlo, y va DELANTE
         * porque es lo que permite saltarse un dominio desconocido.  Se deja el
         * hueco y se rellena al final: armar el cuerpo aparte para medirlo
         * costaria una copia entera de todo el fichero. */
        const size_t pos_longitud = w.size();
        w.u32(0);
        const size_t inicio_cuerpo = w.size();

        /* La tabla de cadenas tampoco se conoce hasta codificar los hechos, asi
         * que va DETRAS y el cuerpo empieza por su desplazamiento.  Misma razon:
         * ni buffers intermedios ni copias. */
        const size_t pos_tabla = w.size();
        w.u32(0);

        StringTable tabla;
        auto         cad = [&](const char *s) { return tabla.index_of(s); };
        for (FactId id : ids) {
            const Fact &f = almacen.at(id);
            w.u32(id); // identidad dentro del fichero, para las pruebas.
            w.u32(cad(f.que.codigo));
            w.i64(f.que.a);
            w.i64(f.que.b);
            w.u32(cad(f.que.detalle));
            w.u8(static_cast<uint8_t>(f.de_quien.clase));
            w.u32(cad(f.de_quien.funcion));
            w.u32(f.de_quien.id);
            w.u8(static_cast<uint8_t>(f.sello.certeza));
            w.u8(static_cast<uint8_t>(f.sello.origen.fuente));
            w.u32(cad(f.sello.origen.productor));
            w.u32(cad(f.sello.origen.funcion));
            w.u32(f.sello.origen.sitio);
            for (int i = 0; i < Dependencias::kMax; ++i) {
                const char *a = f.sello.apoyos.de[i];
                w.u32(a == nullptr ? kNoString : cad(a));
            }
            w.u32(cad(f.prueba.regla));
            w.u32(static_cast<uint32_t>(f.prueba.de.size()));
            for (FactId d : f.prueba.de) w.u32(d);
        }
        w.patch_u32(pos_tabla,
                       static_cast<uint32_t>(w.size() - inicio_cuerpo));
        tabla.escribir(w);
        w.patch_u32(pos_longitud,
                       static_cast<uint32_t>(w.size() - inicio_cuerpo));
        indice.push_back(e);
    }

    const uint64_t offset_indice = w.size();
    for (const IndexEntry &e : indice) {
        w.u64(e.hash);
        w.u64(e.offset);
    }
    w.u64(offset_indice);
    w.u32(kMagicEnd);
    return w.take();
}

ReadResult read_facts(const uint8_t *datos, size_t n, uint64_t huella,
                      FactStore                     &destino,
                      const std::vector<DomainCost> &vigentes) {
    ReadResult r;
    if (datos == nullptr || n == 0) {
        r.reason = ReadReason::Empty;
        return r;
    }
    /* Los nombres conocidos tienen que estar dados de alta ANTES de traducir
     * ninguna cadena, o los hechos volverian con productores que no se
     * reconocen a si mismos. */
    register_asa_canonical_names();
    util::ByteReader L(datos, n);
    if (L.u32() != kMagic) {
        r.reason = ReadReason::NotAFactFile;
        return r;
    }
    const uint16_t ver = L.u16();
    L.u16();
    if (!L.ok()) {
        r.reason = ReadReason::Truncated;
        return r;
    }
    if (ver != kContainerVersion) {
        r.reason = ReadReason::OtherVersion;
        return r;
    }
    /* La huella es lo unico que garantiza que estos hechos hablan de ESTE
     * modulo.  Sin ella, un fichero viejo al lado de un fuente nuevo daria
     * respuestas de otro programa, que es peor que no dar ninguna. */
    if (L.u64() != huella) {
        r.reason = ReadReason::OtherModule;
        return r;
    }
    const uint32_t total_original = L.u32();
    const uint32_t n_registros = L.u32();
    if (!L.ok()) {
        r.reason = ReadReason::Truncated;
        return r;
    }

    /* Los hechos se arman aparte antes de depositarlos porque sus pruebas
     * apuntan a identificadores del FICHERO, y hasta no saber cuales se han
     * cargado de verdad no se pueden traducir. */
    std::vector<Fact>   leidos;
    std::vector<FactId> remapeo(total_original, kSinHecho);
    const FactId        base = static_cast<FactId>(destino.size());

    for (uint32_t i = 0; i < n_registros && L.ok(); ++i) {
        const std::string nombre = L.str();
        const uint16_t    ver_hecho = L.u16();
        const uint64_t    huella_dom = L.u64();
        const uint32_t    n_hechos = L.u32();
        const uint32_t    longitud = L.u32();
        if (!L.ok()) break;
        const size_t inicio_cuerpo = L.position();
        const size_t fin_registro = inicio_cuerpo + longitud;
        if (fin_registro > n) {
            L.seek(n + 1); // rompe el lector: el registro se sale del fichero.
            break;
        }

        /* Un dominio cuyo layout no se reconoce SE SALTA.  Para esto lleva la
         * longitud delante: cambiar el contenido de un dominio descarta lo suyo,
         * no el fichero entero, y nadie tiene que subir una version global para
         * añadir un productor. */
        if (ver_hecho != kFactVersion) {
            ++r.skipped;
            L.seek(fin_registro);
            continue;
        }

        /* Y aqui la granularidad: si el llamante dice de que dependen HOY los
         * hechos de este dominio y no cuadra, se descarta SOLO este registro.
         * Un dominio del que no dice nada -- o que no supo decir de que
         * dependia -- se acepta: no se puede comprobar, y suponer lo peor
         * tiraria una cache que probablemente vale. */
        bool caduco = false;
        if (huella_dom != 0) {
            for (const DomainCost &v : vigentes) {
                if (v.domain == nullptr || nombre != v.domain) continue;
                caduco = v.fingerprint != 0 && v.fingerprint != huella_dom;
                break;
            }
        }
        if (caduco) {
            ++r.stale;
            L.seek(fin_registro);
            continue;
        }

        const char *dominio = canonicalize(destino, nombre);

        /* Las cadenas van al final del cuerpo, asi que se leen antes de los
         * hechos saltando a ellas y volviendo. */
        const uint32_t off_tabla = L.u32();
        if (!L.ok()) break;
        const size_t pos_hechos = L.position();
        L.seek(inicio_cuerpo + off_tabla);
        const uint32_t            n_cadenas = L.u32();
        std::vector<const char *> cadenas;
        cadenas.reserve(n_cadenas);
        for (uint32_t c = 0; c < n_cadenas && L.ok(); ++c)
            cadenas.push_back(canonicalize(destino, L.str()));
        if (!L.ok()) break;
        L.seek(pos_hechos);
        auto cad = [&](uint32_t k) -> const char * {
            return k < cadenas.size() ? cadenas[k] : "";
        };

        leidos.reserve(leidos.size() + n_hechos);
        for (uint32_t h = 0; h < n_hechos && L.ok(); ++h) {
            Fact           f;
            const uint32_t id_original = L.u32();
            f.que.dominio = dominio;
            f.que.codigo = cad(L.u32());
            f.que.a = L.i64();
            f.que.b = L.i64();
            f.que.detalle = cad(L.u32());
            f.de_quien.clase = static_cast<Sujeto::Clase>(L.u8());
            f.de_quien.funcion = cad(L.u32());
            f.de_quien.id = L.u32();
            f.sello.certeza = static_cast<Certeza>(L.u8());
            f.sello.origen.fuente = static_cast<Fuente>(L.u8());
            f.sello.origen.productor = cad(L.u32());
            f.sello.origen.funcion = cad(L.u32());
            f.sello.origen.sitio = L.u32();
            for (int k = 0; k < Dependencias::kMax; ++k) {
                const uint32_t idx = L.u32();
                f.sello.apoyos.de[k] = (idx == kNoString) ? nullptr : cad(idx);
            }
            f.prueba.regla = cad(L.u32());
            const uint32_t n_apoyos = L.u32();
            if (!L.ok()) break;
            f.prueba.de.reserve(n_apoyos);
            for (uint32_t k = 0; k < n_apoyos && L.ok(); ++k)
                f.prueba.de.push_back(L.u32()); // aun en identidades del fichero.
            if (!L.ok()) break;
            if (id_original < remapeo.size())
                remapeo[id_original] = base + static_cast<FactId>(leidos.size());
            leidos.push_back(std::move(f));
        }
        if (!L.ok()) break;
        L.seek(fin_registro);
        ++r.domains;
    }

    if (!L.ok()) {
        r.reason = ReadReason::Truncated;
        return r;
    }

    /* Traducir las pruebas.  Un apoyo cuyo hecho no se cargo -- porque su
     * dominio se salto -- se PIERDE y se cuenta: mejor una derivacion mas corta
     * que una que apunte a un hecho que no existe. */
    for (Fact &f : leidos) {
        size_t escribe = 0;
        for (size_t k = 0; k < f.prueba.de.size(); ++k) {
            const FactId orig = f.prueba.de[k];
            const FactId nuevo =
                (orig < remapeo.size()) ? remapeo[orig] : kSinHecho;
            if (nuevo == kSinHecho) {
                ++r.lost_proofs;
                continue;
            }
            f.prueba.de[escribe++] = nuevo;
        }
        f.prueba.de.resize(escribe);
    }

    destino.reservar(destino.size() + leidos.size());
    for (Fact &f : leidos) destino.anadir(std::move(f));
    r.facts = static_cast<uint32_t>(leidos.size());
    r.ok = true;
    return r;
}

ReadResult read_facts_file(const std::string &ruta, uint64_t huella,
                           FactStore                     &destino,
                           const std::vector<DomainCost> &vigentes) {
    ReadResult     r;
    std::vector<uint8_t> bytes;
    if (!::fs::file_exists(ruta)) {
        r.reason = ReadReason::NoFile;
        return r;
    }
    if (!::fs::read_file_bytes(ruta, bytes)) {
        r.reason = ReadReason::ReadFailed;
        return r;
    }
    if (bytes.empty()) {
        r.reason = ReadReason::Empty;
        return r;
    }
    return read_facts(bytes.data(), bytes.size(), huella, destino, vigentes);
}

} // namespace asa
} // namespace analysis
