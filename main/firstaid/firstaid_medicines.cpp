/*
 * @Description: First Aid guide: medicines (with the doses from their Czech
 *               package leaflets) and supplies for the cabinet
 * @License: GPL 3.0
 *
 * Doses are those printed in the leaflets of the usual over-the-counter
 * products in Czechia (SÚKL SPC). The phone shows them as a guide next to the
 * leaflet, never instead of it.
 */
#include <cstring>

#include "firstaid/firstaid_content.h"

namespace lilygo_box::firstaid {
namespace {

using F = Form;

constexpr Medicine kMedicines[] = {
{"paracetamol", "Paracetamol", "Paralen, Panadol, Paracetamol, Efferalgan, Coldrex",
 "horecka bolest bolest hlavy bolest zubu bolest v krku nachlazeni chripka", F::kTablet, 500,
 15, 60, 1000, 4000, 6, 3, 50,
 "Děti: 10–15 mg na kg váhy jednou, nejdříve po 6 hodinách, nejvýš 60 mg/kg za den.\n"
 "Dospělí a děti nad 50 kg: 500–1000 mg, nejdříve po 4–6 hodinách, nejvýš 4000 mg za den.",
 "Ne při onemocnění jater a s alkoholem. Hlídej, zda paracetamol není i v jiném léku (Coldrex, Theraflu) – sečti dávky. Kojenci do 3 měsíců jen po poradě s lékařem."},
{"ibuprofen", "Ibuprofen", "Ibalgin, Nurofen, Brufen, Ibuprofen",
 "bolest horecka zanet otok bolest zubu bolest zad menstruace vyron", F::kTablet, 400,
 10, 30, 400, 1200, 6, 3, 40,
 "Děti od 3 měsíců a 5 kg: 5–10 mg na kg jednou, po 6–8 hodinách, nejvýš 30 mg/kg za den.\n"
 "Dospělí a děti nad 40 kg: 200–400 mg po 4–6 hodinách, nejvýš 1200 mg za den bez lékaře.",
 "Ne při vředech a krvácení do žaludku, odvodnění, onemocnění ledvin, ve 3. trimestru těhotenství, při astmatu po ibuprofenu. Užívat s jídlem."},
{"acetylsalicylic", "Kyselina acetylsalicylová", "Aspirin, Acylpyrin, Anopyrin",
 "bolest horecka infarkt bolest na hrudi bolest hlavy", F::kTablet, 500,
 0, 0, 500, 3000, 4, 192, 50,
 "Dospělí: 500–1000 mg po 4–8 hodinách, nejvýš 3000 mg za den. Podezření na infarkt: 1 tabletu 250–500 mg rozkousat.",
 "Ne dětem a mladistvým do 16 let (Reyeův syndrom). Ne při vředech, krvácení, s léky na ředění krve, v těhotenství, při alergii a astmatu po aspirinu."},
{"cetirizine", "Cetirizin", "Zyrtec, Cetixin, Analergin, Zodac",
 "alergie koprivka senna ryma svedeni bodnuti hmyzem otok alergicka reakce", F::kTablet, 10,
 0, 0, 10, 10, 24, 24, 30,
 "Dospělí a děti od 12 let: 10 mg jednou denně.\nDěti 6–12 let: 5 mg dvakrát denně.\nDěti 2–6 let: 2,5 mg (kapky) dvakrát denně.",
 "Může způsobit ospalost – opatrně za volantem a s alkoholem. Ne při těžkém onemocnění ledvin."},
{"loratadine", "Loratadin", "Claritine, Flonidan, Loratadin",
 "alergie koprivka senna ryma svedeni", F::kTablet, 10,
 0, 0, 10, 10, 24, 24, 30,
 "Dospělí a děti od 12 let nebo nad 30 kg: 10 mg jednou denně.\nDěti 2–12 let do 30 kg: 5 mg jednou denně.",
 "Méně ospalý než starší léky na alergii. Ne dětem do 2 let."},
{"loperamide", "Loperamid", "Imodium, Loperon",
 "prujem ridka stolice", F::kCapsule, 2,
 0, 0, 2, 12, 2, 144, 40,
 "Dospělí a děti od 12 let: na začátku 4 mg, pak 2 mg po každé řídké stolici, nejvýš 12 mg za den, nejdéle 2 dny.",
 "Ne dětem do 12 let. Ne při horečce, krvi ve stolici, po antibiotikách. Hlavní léčba průjmu je pití."},
{"activated_charcoal", "Živočišné uhlí", "Carbo medicinalis, Carbosorb",
 "prujem otrava nadymani otrava jidlem", F::kTablet, 250,
 0, 0, 1000, 4000, 6, 24, 40,
 "Průjem: dospělí 2–4 tablety 3–4× denně, děti od 2 let 1–2 tablety 3× denně.\n"
 "Otrava: jen na radu lékaře nebo TIS (obvykle 0,5–1 g na kg váhy jednorázově).",
 "Užívat s odstupem 2 hodiny od jiných léků – zachytí je. Ne při otravě žíravinami a benzínem."},
{"ors", "Rehydratační roztok", "Kulíšek, Normolyt, Enterodetox, ORS",
 "prujem zvraceni dehydratace odvodneni horko upal", F::kPowder, 0,
 0, 0, 0, 0, 0, 0, 0,
 "1 sáček rozpusť v množství vody podle obalu.\nDítě: 50–100 ml na kg váhy během 4 hodin, pak 10 ml/kg po každé řídké stolici.\nDospělý: 200–400 ml po každé řídké stolici.\nPít po malých doušcích.",
 "Nepřipravuj silnější roztok, než je na obalu."},
{"ambroxol", "Ambroxol", "Mucosolvan, Ambrobene, Ambroxol",
 "kasel vlhky kasel hlen odkasavani", F::kTablet, 30,
 0, 0, 30, 90, 8, 24, 40,
 "Dospělí a děti od 12 let: 30 mg 3× denně 2–3 dny, pak 2× denně.\nDěti 6–12 let: 15 mg 2–3× denně.\nDěti 2–5 let: 7,5 mg 3× denně (sirup).",
 "Užívat přes den, ne před spaním. Nekombinovat s léky proti kašli."},
{"acetylcysteine", "Acetylcystein", "ACC, Solmucol, Fluimucil",
 "kasel vlhky kasel hlen", F::kTablet, 200,
 0, 0, 200, 600, 8, 24, 40,
 "Dospělí a děti od 14 let: 200 mg 2–3× denně nebo 600 mg jednou denně.\nDěti 6–14 let: 100 mg 3× nebo 200 mg 2× denně.\nDěti 2–5 let: 100 mg 2–3× denně.",
 "Užívat přes den. Nekombinovat s léky proti kašli. Opatrně při astmatu."},
{"butamirate", "Butamirát (lék proti kašli)", "Sinecod, Stoptussin",
 "suchy kasel drazdivy kasel kasel v noci", F::kSyrup, 1.5,
 0, 0, 0, 0, 8, 36, 0,
 "Dávkuj přesně podle letáku a váhy – sirup a kapky se liší silou.",
 "Jen na suchý dráždivý kašel, ne při hlenu. Ne dětem do 3 let bez lékaře (kapky od 2 měsíců jen dle lékaře)."},
{"dextromethorphan", "Dextromethorfan (lék proti kašli)", "Robitussin Junior/Antitussicum, Stopex",
 "suchy kasel drazdivy kasel", F::kSyrup, 1.5,
 0, 0, 15, 120, 6, 144, 40,
 "Dospělí a děti od 12 let: 15–30 mg po 6–8 hodinách, nejvýš 120 mg za den.",
 "Ne dětem do 12 let bez lékaře, ne s antidepresivy (IMAO), ne při astmatu a hlenu."},
{"xylometazoline", "Xylometazolin (nosní kapky)", "Olynth, Otrivin, Nasivin (oxymetazolin)",
 "ryma ucpany nos nachlazeni krvaceni z nosu", F::kSpray, 0,
 0, 0, 0, 0, 8, 24, 0,
 "Dospělí a děti od 12 let: 0,1 %, 1 vstřik do každé nosní dírky nejvýš 3× denně.\nDěti 2–12 let: 0,05 % (dětská verze).\nNejdéle 5–7 dní.",
 "Déle než týden poškozuje sliznici. Ne kojencům bez lékaře."},
{"simethicone", "Simetikon", "Espumisan, Sab simplex",
 "nadymani plynatost bolest bricha", F::kCapsule, 40,
 0, 0, 80, 600, 4, 0, 40,
 "Dospělí: 2 tobolky (80 mg) 3–5× denně. Děti 6–14 let: 1–2 tobolky 3–5× denně. Kojenci: kapky podle letáku.",
 ""},
{"antacid", "Antacidum (na pálení žáhy)", "Rennie, Gaviscon, Anacid",
 "paleni zahy zaha kyselina", F::kTablet, 0,
 0, 0, 0, 0, 2, 144, 0,
 "1–2 tablety rozkousat při potížích, nejvýš podle letáku (obvykle 10–11 tablet za den).",
 "S odstupem 2 hodin od jiných léků. Ne dlouhodobě bez lékaře."},
{"dimenhydrinate", "Lék proti nevolnosti z cestování", "Kinedryl, Travel-Gum",
 "nevolnost pri cestovani kinetoza nevolno v aute", F::kTablet, 25,
 0, 0, 25, 100, 6, 72, 40,
 "Podávej 30–60 minut před cestou, dávkuj podle letáku (Kinedryl: dospělí 1–2 tablety, děti od 6 let 1/2–1 tableta).",
 "Způsobuje ospalost. Ne řidičům."},
{"diclofenac_gel", "Diklofenak gel", "Voltaren Emulgel, Diclofenac gel",
 "bolest kloubu vyron podvrtnuti bolest zad pohmozdenina", F::kCream, 0,
 0, 0, 0, 0, 6, 168, 0,
 "Tence vetřít 3–4× denně do bolavého místa, nejdéle 2 týdny. Od 14 let.",
 "Ne na poraněnou kůži, ne v posledních měsících těhotenství. Po nanesení si umyj ruce."},
{"hydrocortisone", "Hydrokortizon krém 1 %", "Hydrocortison krém, Pragocort (Rx)",
 "svedeni kopřivka bodnuti hmyzem ekzem vyrazka alergie", F::kCream, 0,
 0, 0, 0, 0, 12, 72, 0,
 "Tence 1–2× denně na malou plochu, nejdéle 7 dní.",
 "Ne na infikovanou kůži, plíseň, opar, akné, ne pod obvaz. U dětí ne na obličej."},
{"clotrimazole", "Klotrimazol krém", "Canesten, Clotrimazol",
 "plisen svedeni mezi prsty plisen v trislech", F::kCream, 0,
 0, 0, 0, 0, 8, 0, 0,
 "2–3× denně na postižené místo, ještě 2 týdny po zhojení (celkem obvykle 3–4 týdny).",
 ""},
{"zinc_ointment", "Zinková mast", "Zinková mast, Sudocrem",
 "opruzeniny vyrazka plenky", F::kCream, 0,
 0, 0, 0, 0, 0, 0, 0,
 "Na suchou čistou kůži několikrát denně.",
 ""},
{"panthenol", "Panthenol (dexpanthenol)", "Panthenol spray, Bepanthen",
 "popalenina spaleni od slunce odrenina rana hojeni", F::kSpray, 0,
 0, 0, 0, 0, 0, 0, 0,
 "Na zchlazenou popáleninu nebo hojící se ránu 1–3× denně.",
 "Ne na hluboké a rozsáhlé popáleniny – ty patří lékaři."},
{"octenidine", "Oktenidin (antiseptikum)", "Octenisept",
 "rana odrenina dezinfekce kliste triska puchyr", F::kSolution, 0,
 0, 0, 0, 0, 0, 0, 0,
 "Postříkej nebo potři ránu, nech 1 minutu působit. Lze i na sliznice a u dětí.",
 "Nevstřikovat pod tlakem do hlubokých ran."},
{"povidone_iodine", "Povidon-jód (antiseptikum)", "Betadine, Jodisol",
 "rana odrenina dezinfekce kousnuti kliste", F::kSolution, 0,
 0, 0, 0, 0, 0, 0, 0,
 "Potři ránu a okolí, nech zaschnout.",
 "Ne při onemocnění štítné žlázy, alergii na jód, v těhotenství a kojení dlouhodobě, ne u kojenců do 6 měsíců."},
{"chlorhexidine", "Chlorhexidin (antiseptikum)", "Septonex, Hibiclens, Chlorhexidin",
 "rana dezinfekce odrenina", F::kSolution, 0,
 0, 0, 0, 0, 0, 0, 0,
 "Potři ránu a okolí.",
 "Ne do oka a ucha."},
{"salbutamol", "Salbutamol inhalátor (na předpis)", "Ventolin, Buventol",
 "astma dusnost piskot", F::kSpray, 0.1,
 0, 0, 0.1, 0, 0, 0, 0,
 "Záchvat: 1 vdech každých 30–60 s, až 10 vdechů, nejlépe přes nástavec. Bez zlepšení 155.",
 "Jen pro toho, komu ho předepsal lékař."},
{"epinephrine", "Adrenalinové pero (na předpis)", "EpiPen, Jext, Emerade",
 "anafylaxe alergicka reakce otok jazyka bodnuti vosa", F::kOther, 0.3,
 0, 0, 0.3, 0, 0, 0, 30,
 "Do zevní strany stehna, i přes oblečení. 0,3 mg od 30 kg, 0,15 mg pro děti 15–30 kg. Po 5–15 minutách bez zlepšení druhé pero. Vždy 155.",
 "Pro toho, komu bylo předepsáno; v ohrožení života ho lze použít."},
};

constexpr Supply kSupplies[] = {
    {"Sterilní krytí (gáza)", F::kBandage, "rana krvaceni popalenina"},
    {"Obinadlo hydrofilní", F::kBandage, "obvaz rana"},
    {"Obinadlo elastické", F::kBandage, "vyron podvrtnuti obvaz"},
    {"Obvaz s polštářkem", F::kBandage, "tlakovy obvaz krvaceni"},
    {"Náplasti", F::kBandage, "rana odrenina"},
    {"Náplast na puchýře", F::kBandage, "puchyr"},
    {"Náplasťové stehy (steri-strip)", F::kBandage, "rezna rana"},
    {"Náplast v roli", F::kBandage, "obvaz"},
    {"Trojcípý šátek", F::kBandage, "zaves zlomenina"},
    {"Nepřilnavé krytí na popáleniny", F::kBandage, "popalenina"},
    {"Dlaha (SAM)", F::kBandage, "zlomenina dlaha"},
    {"Škrtidlo (turniket)", F::kTool, "silne krvaceni"},
    {"Jednorázové rukavice", F::kTool, "ochrana"},
    {"Resuscitační rouška", F::kTool, "resuscitace"},
    {"Izotermická fólie", F::kTool, "podchlazeni sok"},
    {"Nůžky", F::kTool, "obvaz"},
    {"Pinzeta", F::kTool, "triska kliste"},
    {"Karta na klíšťata", F::kTool, "kliste"},
    {"Teploměr", F::kTool, "horecka"},
    {"Chladivý sáček", F::kTool, "vyron otok"},
    {"Fyziologický roztok", F::kSolution, "vyplach oka rana"},
    {"Zavírací špendlíky", F::kTool, "obvaz"},
};

}  // namespace

size_t MedicineCount() { return sizeof(kMedicines) / sizeof(kMedicines[0]); }

const Medicine& MedicineAt(size_t index) { return kMedicines[index]; }

const Medicine* FindMedicine(const char* key) {
  if (key == nullptr || key[0] == '\0') {
    return nullptr;
  }
  for (const Medicine& medicine : kMedicines) {
    if (std::strcmp(medicine.key, key) == 0) {
      return &medicine;
    }
  }
  return nullptr;
}

size_t SupplyCount() { return sizeof(kSupplies) / sizeof(kSupplies[0]); }

const Supply& SupplyAt(size_t index) { return kSupplies[index]; }

const char* FormName(Form form) {
  switch (form) {
    case Form::kTablet: return "tablety";
    case Form::kCapsule: return "tobolky";
    case Form::kSyrup: return "sirup";
    case Form::kDrops: return "kapky";
    case Form::kCream: return "krém, gel, mast";
    case Form::kSpray: return "sprej";
    case Form::kSolution: return "roztok";
    case Form::kPowder: return "prášek, sáčky";
    case Form::kSuppository: return "čípky";
    case Form::kBandage: return "obvazový materiál";
    case Form::kTool: return "pomůcka";
    case Form::kOther: return "jiné";
    case Form::kCount: break;
  }
  return "";
}

}  // namespace lilygo_box::firstaid
