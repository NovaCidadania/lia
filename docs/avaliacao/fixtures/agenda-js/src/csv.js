// CSV simples com ';' como separador.
export function parseCsv(texto) {
  return texto
    .split('\n')
    .filter((l) => l !== '')
    .map((l) => l.split(';'))
}
