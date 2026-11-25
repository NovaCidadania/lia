// Datas no formato brasileiro dd/mm/aaaa.
export function parseDataBR(texto) {
  const [d, m, a] = String(texto).split('/').map(Number)
  return new Date(Date.UTC(a, m - 1, d))
}

export function formatarDataBR(data) {
  const p = (n) => String(n).padStart(2, '0')
  return `${p(data.getUTCDate())}/${p(data.getUTCMonth() + 1)}/${data.getUTCFullYear()}`
}
